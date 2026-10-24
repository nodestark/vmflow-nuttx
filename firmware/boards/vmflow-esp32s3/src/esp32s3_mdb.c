/****************************************************************************
 * firmware/boards/vmflow-esp32s3/src/esp32s3_mdb.c
 *
 * MDB bus driver (/dev/mdb0): 9600 baud, 9-bit words (8 data bits plus the
 * mode bit), bit-banged on two GPIOs like mdb-slave-esp32s3.c in the
 * ESP-IDF firmware. A falling edge on RX is a start bit; the interrupt
 * samples the word in place and queues it. read() and write() move
 * uint16_t words, bit 8 being the mode bit; poll() reports POLLIN while a
 * word is queued.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/fs/fs.h>
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>

#include "xtensa.h"
#include "espressif/esp_gpio.h"
#include "soc/gpio_reg.h"
#include "hardware/esp32s3_gpio_sigmap.h"

#include "vmflow-esp32s3.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MDB_RX_PIN    CONFIG_VMFLOW_MDB_RX_GPIO
#define MDB_TX_PIN    CONFIG_VMFLOW_MDB_TX_GPIO

#define MDB_BIT_US    104         /* 9600 baud */
#define MDB_WORD_BITS 9

#define MDB_RXBUF     32          /* words, power of two */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct mdb_dev_s
{
  uint16_t rxbuf[MDB_RXBUF];
  volatile uint8_t head;          /* written by the interrupt */
  volatile uint8_t tail;          /* written by read() */
  sem_t rxsem;                    /* one count per queued word */
  FAR struct pollfd *fds;         /* the engine's poll(), if waiting */
  bool open;
  uint32_t overruns;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int     mdb_open(FAR struct file *filep);
static int     mdb_close(FAR struct file *filep);
static ssize_t mdb_read(FAR struct file *filep, FAR char *buffer,
                        size_t buflen);
static ssize_t mdb_write(FAR struct file *filep, FAR const char *buffer,
                         size_t buflen);
static int     mdb_poll(FAR struct file *filep, FAR struct pollfd *fds,
                        bool setup);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_mdb_fops =
{
  .open  = mdb_open,
  .close = mdb_close,
  .read  = mdb_read,
  .write = mdb_write,
  .poll  = mdb_poll,
};

static struct mdb_dev_s g_mdb;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

extern void esp_rom_delay_us(uint32_t us);

/****************************************************************************
 * Name: mdb_rx_interrupt
 *
 * Description:
 *   Falling edge on RX: wait 1.5 bit to land in the middle of bit 0, then
 *   sample the nine data bits one bit apart. Returns in the stop bit.
 *
 ****************************************************************************/

static int mdb_rx_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct mdb_dev_s *priv = arg;
  uint16_t word = 0;
  uint8_t next;
  int i;

  esp_rom_delay_us(MDB_BIT_US + MDB_BIT_US / 2);
  for (i = 0; i < MDB_WORD_BITS; i++)
    {
      word |= (uint16_t)esp_gpioread(MDB_RX_PIN) << i;
      esp_rom_delay_us(MDB_BIT_US);
    }

  /* The GPIO dispatcher clears an edge interrupt before calling us, so the
   * data bits just sampled latched it again: clear it, or it fires on a
   * word that is not there.
   */

  putreg32(1 << MDB_RX_PIN, GPIO_STATUS_W1TC_REG);

  next = (priv->head + 1) & (MDB_RXBUF - 1);
  if (next == priv->tail)
    {
      priv->overruns++;
      return OK;
    }

  priv->rxbuf[priv->head] = word;
  priv->head = next;
  nxsem_post(&priv->rxsem);
  poll_notify(&priv->fds, 1, POLLIN);
  return OK;
}

/****************************************************************************
 * Name: mdb_tx_word
 *
 * Description:
 *   Start bit, nine data bits LSB first, stop bit. Interrupts stay off for
 *   the ~1.15 ms of one word so the bit timing holds.
 *
 ****************************************************************************/

static void mdb_tx_word(uint16_t word)
{
  irqstate_t flags;
  int i;

  flags = enter_critical_section();

  esp_gpiowrite(MDB_TX_PIN, false);
  esp_rom_delay_us(MDB_BIT_US);

  for (i = 0; i < MDB_WORD_BITS; i++)
    {
      esp_gpiowrite(MDB_TX_PIN, (word >> i) & 1);
      esp_rom_delay_us(MDB_BIT_US);
    }

  esp_gpiowrite(MDB_TX_PIN, true);
  esp_rom_delay_us(MDB_BIT_US);

  leave_critical_section(flags);
}

static int mdb_open(FAR struct file *filep)
{
  FAR struct mdb_dev_s *priv = filep->f_inode->i_private;
  irqstate_t flags;

  /* One bus, one engine */

  flags = enter_critical_section();
  if (priv->open)
    {
      leave_critical_section(flags);
      return -EBUSY;
    }

  priv->open = true;
  priv->head = 0;
  priv->tail = 0;
  nxsem_reset(&priv->rxsem, 0);
  leave_critical_section(flags);

  putreg32(1 << MDB_RX_PIN, GPIO_STATUS_W1TC_REG);
  esp_gpioirqenable(MDB_RX_PIN);
  return OK;
}

static int mdb_close(FAR struct file *filep)
{
  FAR struct mdb_dev_s *priv = filep->f_inode->i_private;

  esp_gpioirqdisable(MDB_RX_PIN);
  priv->open = false;
  return OK;
}

/****************************************************************************
 * Name: mdb_read
 *
 * Description:
 *   Blocks until at least one word arrived (unless O_NONBLOCK), then returns
 *   as many queued words as fit.
 *
 ****************************************************************************/

static ssize_t mdb_read(FAR struct file *filep, FAR char *buffer,
                        size_t buflen)
{
  FAR struct mdb_dev_s *priv = filep->f_inode->i_private;
  size_t count = 0;
  uint16_t word;
  int ret;

  if (buflen < sizeof(uint16_t))
    {
      return -EINVAL;
    }

  if ((filep->f_oflags & O_NONBLOCK) != 0)
    {
      ret = nxsem_trywait(&priv->rxsem);
      if (ret < 0)
        {
          return -EAGAIN;
        }
    }
  else
    {
      ret = nxsem_wait(&priv->rxsem);
      if (ret < 0)
        {
          return ret;
        }
    }

  do
    {
      word = priv->rxbuf[priv->tail];
      priv->tail = (priv->tail + 1) & (MDB_RXBUF - 1);
      memcpy(buffer + count, &word, sizeof(word));
      count += sizeof(word);
    }
  while (count + sizeof(word) <= buflen &&
         nxsem_trywait(&priv->rxsem) == OK);

  return count;
}

static ssize_t mdb_write(FAR struct file *filep, FAR const char *buffer,
                         size_t buflen)
{
  uint16_t word;
  size_t i;

  for (i = 0; i + sizeof(word) <= buflen; i += sizeof(word))
    {
      memcpy(&word, buffer + i, sizeof(word));
      mdb_tx_word(word);
    }

  return i;
}

static int mdb_poll(FAR struct file *filep, FAR struct pollfd *fds,
                    bool setup)
{
  FAR struct mdb_dev_s *priv = filep->f_inode->i_private;
  irqstate_t flags;
  int ret = OK;

  flags = enter_critical_section();

  if (setup)
    {
      if (priv->fds != NULL)
        {
          ret = -EBUSY;
        }
      else
        {
          priv->fds = fds;
          fds->priv = &priv->fds;
          if (priv->head != priv->tail)
            {
              poll_notify(&priv->fds, 1, POLLIN);
            }
        }
    }
  else if (fds->priv != NULL)
    {
      *(FAR struct pollfd **)fds->priv = NULL;
      fds->priv = NULL;
    }

  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_mdb_initialize
 *
 * Description:
 *   Idle the TX line high, arm a falling-edge interrupt on RX (enabled on
 *   open) and register /dev/mdb0.
 *
 ****************************************************************************/

int board_mdb_initialize(void)
{
  int ret;

  nxsem_init(&g_mdb.rxsem, 0, 0);

  esp_gpio_matrix_out(MDB_TX_PIN, SIG_GPIO_OUT_IDX, 0, 0);
  esp_configgpio(MDB_TX_PIN, OUTPUT_FUNCTION_2);
  esp_gpiowrite(MDB_TX_PIN, true);

  esp_configgpio(MDB_RX_PIN, INPUT_FUNCTION_2 | PULLUP | FALLING);

  ret = esp_gpio_irq(MDB_RX_PIN, mdb_rx_interrupt, &g_mdb);
  if (ret < 0)
    {
      return ret;
    }

  esp_gpioirqdisable(MDB_RX_PIN);

  return register_driver("/dev/mdb0", &g_mdb_fops, 0666, &g_mdb);
}
