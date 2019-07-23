/* AT86RF215 driver */

/*
 * General Explanations:
 *** spi_device :  is used to interchange data between an SPI slave
 * (usually a discrete chip) and CPU memory.
 *** platform_data : Linux uses platform_data to point to board-specific structures describing devices and how they are connected to the SoC.
 * In our case, it's the spi platform : &spi0
 *** dev.of_node : associated device tree node .
 * In our case, it's the node of : at86rf215@0{ .
 */

/*
 * Notes:
 * - I should check the sleep_gpio and reset_gpio in device tree later.
 *
*/

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
//#include <linux/spi/at86rf230.h>
#include <linux/regmap.h>
#include <linux/skbuff.h>
#include <linux/of_gpio.h>
#include <linux/ieee802154.h>
#include <linux/debugfs.h>
#include <net/mac802154.h>
#include <net/cfg802154.h>
#include "at86rf215.h"

//The following structure is used by the function "at86rf215_get_pdata" and was declared in <linux/spi/at86rf230.h> for AT86RF230 driver.
struct at86rf215_platform_data {
        int rstn;
        int slp_tr;
        int dig2;
        u8 xtal_trim;
};

struct at86rf215_local;
/* at86rf2xx chip depend data.
 * All timings are in us.
 */

struct at86rf215_chip_data {
        u16 t_sleep_cycle;
        u16 t_channel_switch;
        u16 t_reset_to_off;
        u16 t_off_to_aack;
        u16 t_off_to_tx_on;
        u16 t_off_to_sleep;
        u16 t_sleep_to_off;
        u16 t_frame;
        u16 t_p_ack;
        int rssi_base_val;

        int (*set_channel)(struct at86rf215_local *, u8, u8);
        int (*set_txpower)(struct at86rf215_local *, s32);
};

#define AT86RF215_MAX_BUF		(127 + 3)
/* tx retries to access the TX_ON state // RG_TRX_STATE : 0x02
 * if it's above then force change will be started.
 *
 * We assume the max_frame_retries (7) value of 802.15.4 here.
 */
#define AT86RF215_MAX_TX_RETRIES	7
/* We use the recommended 5 minutes timeout to recalibrate */
#define AT86RF215_CAL_LOOP_TIMEOUT	(5 * 60 * HZ)

struct at86rf215_state_change {
	struct at86rf215_local *lp;
	int irq;

	struct hrtimer timer;
	struct spi_message msg;
	struct spi_transfer trx;
	u8 buf[AT86RF215_MAX_BUF];

	void (*complete)(void *context);
	u8 from_state;
	u8 to_state;

	bool free;
};

struct at86rf215_trac {
	u64 success;
	u64 success_data_pending;
	u64 success_wait_for_ack;
	u64 channel_access_failure;
	u64 no_ack;
	u64 invalid;
};

struct at86rf215_local {
	struct spi_device *spi;

	struct ieee802154_hw *hw;
	struct at86rf215_chip_data *data;
	struct regmap *regmap;
	int slp_tr;
	bool sleep;

	struct completion state_complete;
	struct at86rf215_state_change state;

	unsigned long cal_timeout;
	bool is_tx;
	bool is_tx_from_off;
	u8 tx_retry;
	struct sk_buff *tx_skb;
	struct at86rf215_state_change tx;

	struct at86rf215_trac trac;
};

static void at86rf215_async_state_change(struct at86rf215_local *lp, struct at86rf215_state_change *ctx, const u8 state, void (*complete)(void *context));

static inline void at86rf215_sleep(struct at86rf215_local *lp)
{
        if (gpio_is_valid(lp->slp_tr)) {
                gpio_set_value(lp->slp_tr, 1);
                usleep_range(lp->data->t_off_to_sleep,
                             lp->data->t_off_to_sleep + 10);
                lp->sleep = true;
        }
}

static inline void at86rf215_awake(struct at86rf215_local *lp)
{
        if (gpio_is_valid(lp->slp_tr)) {
                gpio_set_value(lp->slp_tr, 0);
                usleep_range(lp->data->t_sleep_to_off,
                             lp->data->t_sleep_to_off + 100);
                lp->sleep = false;
        }
}

static inline int __at86rf215_write(struct at86rf215_local *lp, unsigned int addr, unsigned int data)
{
        bool sleep = lp->sleep;
        int ret;

        /* awake for register setting if sleep */
        if (sleep)
                at86rf215_awake(lp);

        ret = regmap_write(lp->regmap, addr, data);

        /* sleep again if was sleeping */
        if (sleep)
                at86rf215_sleep(lp);

        return ret;
}


static inline int __at86rf215_read(struct at86rf215_local *lp, unsigned int addr, unsigned int *data)
{
        bool sleep = lp->sleep;
        int ret;

        /* awake for register setting if sleep */
        if (sleep)
                at86rf215_awake(lp);

        ret = regmap_read(lp->regmap, addr, data);

        /* sleep again if was sleeping */
        if (sleep)
                at86rf215_sleep(lp);

        return ret;
}

static inline int at86rf215_read_subreg(struct at86rf215_local *lp, unsigned int addr, unsigned int mask, unsigned int shift, unsigned int *data)
{
        int rc;

        rc = __at86rf215_read(lp, addr, data);
        if (!rc)
                *data = (*data & mask) >> shift;

        return rc;
}

static inline int at86rf215_write_subreg(struct at86rf215_local *lp, unsigned int addr, unsigned int mask, unsigned int shift, unsigned int data)
{
        bool sleep = lp->sleep;
        int ret;

        /* awake for register setting if sleep */
        if (sleep)
                at86rf215_awake(lp);

        ret = regmap_update_bits(lp->regmap, addr, mask, data << shift);

        /* sleep again if was sleeping */
        if (sleep)
                at86rf215_sleep(lp);

        return ret;
}

static inline void at86rf215_slp_tr_rising_edge(struct at86rf215_local *lp)
{
        gpio_set_value(lp->slp_tr, 1);
        udelay(1);
        gpio_set_value(lp->slp_tr, 0);
}


static bool at86rf215_reg_writeable(struct device *dev, unsigned int reg)
{
        switch (reg) {
        case RG_RF09_CMD:
	case RG_RF09_STATE: // a verifier
                return true;
        default:
                return false;
        }
} // stiiiiill

static bool at86rf215_reg_readable(struct device *dev, unsigned int reg)
{
        bool rc;

        /* all writeable are also readable */
        rc = at86rf215_reg_writeable(dev, reg);
        if (rc)
                return rc;

        /* readonly regs */
        switch (reg) {
        case RG_RF_PN :
        case RG_RF_VN :
        case RG_RF09_CMD:
        case RG_BBC0_IRQS:
	case RG_RF09_IRQS:
        case RG_PHY_RSSI:
                return true;
        default:
                return false;
        }
} //done

static bool at86rf215_reg_volatile(struct device *dev, unsigned int reg)
{
        /* can be changed during runtime */
        switch (reg) {
        case RG_RF09_CMD:
        case RG_RF09_STATE:
        case RG_RF09_IRQS:
	case RG_BBC0_IRQS:
                return true;
        default:
                return false;
        }
} // still

static bool at86rf215_reg_precious(struct device *dev, unsigned int reg)
{
        /* don't clear irq line on read */
        switch (reg) {
        case RG_RF09_IRQS:
        case RG_BBC0_IRQS:
                return true;
        default:
                return false;
        }
} //done


#define AT86RF215_NUMREGS 0x12E
static const struct regmap_config at86rf215_regmap_spi_config = {
        .reg_bits = 16,
        .val_bits = 8,
        .write_flag_mask = CMD_WRITE,
        .read_flag_mask = CMD_READ,
        .cache_type = REGCACHE_RBTREE, /* Red Black tree algorithm */
        .max_register = AT86RF215_NUMREGS,
        .writeable_reg = at86rf215_reg_writeable,
        .readable_reg = at86rf215_reg_readable,
        .volatile_reg = at86rf215_reg_volatile,
        .precious_reg = at86rf215_reg_precious,
};

static void at86rf215_async_error_recover_complete(void *context)
{
        struct at86rf215_state_change *ctx = context;
        struct at86rf215_local *lp = ctx->lp;

        if (ctx->free)
                kfree(ctx);

        ieee802154_wake_queue(lp->hw);
}

static void at86rf215_async_error_recover(void *context)
{
        struct at86rf215_state_change *ctx = context;
        struct at86rf215_local *lp = ctx->lp;

        printk(KERN_DEBUG "The function 'async_error_recover' starts.");
        lp->is_tx = 0;
        at86rf215_async_state_change(lp, ctx, STATE_RF_RX, at86rf215_async_error_recover_complete); /* Previously : STATE_RX_AACK_ON */
}

static inline void at86rf215_async_error(struct at86rf215_local *lp, struct at86rf215_state_change *ctx, int rc)
{
        dev_err(&lp->spi->dev, "spi_async error %d\n", rc);

        at86rf215_async_state_change(lp, ctx, STATE_RF_TRXOFF, at86rf215_async_error_recover);
}

/* The parameter reg was a u8 type previously
 */
static void at86rf215_async_write_reg(struct at86rf215_local *lp, u16 reg, u8 val, struct at86rf215_state_change *ctx, void (*complete)(void *context))
{
        int rc;

        //printk(KERN_DEBUG "The function 'async_write_reg' starts.");
        ctx->buf[0] = ((reg & CMD_REG_MSB)>>8)| CMD_WRITE;
	ctx->buf[1] = reg & CMD_REG_LSB;
        ctx->buf[2] = val;
        //printk(KERN_DEBUG "buf[0]=%x buf[1]=%x buf[2]=%x",ctx->buf[0],ctx->buf[1],ctx->buf[2]);
        ctx->msg.complete = complete;
        rc = spi_async(lp->spi, &ctx->msg);
        if (rc) {
		//printk(KERN_DEBUG "spi_async failed in write_reg.");
		at86rf215_async_error(lp, ctx, rc);
	}
}

/* Generic function to get some register value in async mode 
 */
static void at86rf215_async_read_reg(struct at86rf215_local *lp, u16 reg, struct at86rf215_state_change *ctx, void (*complete)(void *context))
{
        int rc;

        u8 *tx_buf = ctx->buf;

	//printk(KERN_DEBUG "The function 'async_read_reg' starts");
        tx_buf[0] = ((reg & CMD_REG_MSB)>>8);
        tx_buf[1] = reg & CMD_REG_LSB;
        //printk(KERN_DEBUG "buf[0]=%x buf[1]=%x",ctx->buf[0],ctx->buf[1]);
        ctx->msg.complete = complete;

        rc = spi_async(lp->spi, &ctx->msg);
        if (rc){
		//printk(KERN_DEBUG "spi_async failed in read_reg.");
                at86rf215_async_error(lp, ctx, rc);
	}
}


/* Assert state change */
/* If :
 *  - we reached the "MAX TX RETRIES"
 *  - we want to reach either "STATE_TX_ON" or "STATE_RX_ON state".
 *  ==> We switch to "STATE_FORCE_TRX_OFF"
*/
/* NOTE : The following function may be deleted later, or not. */
static void at86rf215_async_state_assert(void *context)
{
        struct at86rf215_state_change *ctx = context;
        struct at86rf215_local *lp = ctx->lp;
        const u8 *buf = ctx->buf;
        const u8 trx_state = buf[1] & TRX_STATE_MASK;

        if (trx_state != ctx->to_state) {
		//printk (KERN_DEBUG "trx_state != ctx->to_state");

                dev_warn(&lp->spi->dev, "unexcept state change from 0x%02x to 0x%02x. Actual state: 0x%02x\n",
                         ctx->from_state, ctx->to_state, trx_state);
	}

}

static enum hrtimer_restart at86rf215_async_state_timer(struct hrtimer *timer)
{
        struct at86rf215_state_change *ctx = container_of(timer, struct at86rf215_state_change, timer);
        struct at86rf215_local *lp = ctx->lp;
	//printk (KERN_DEBUG "The function 'at86rf215_async_state_timer' starts");

        at86rf215_async_read_reg(lp, RG_RF09_CMD, ctx, at86rf215_async_state_assert);

        return HRTIMER_NORESTART;
}

/* Do state change timing delay. */
static void at86rf215_async_state_delay(void *context)
{
        struct at86rf215_state_change *ctx = context;
        struct at86rf215_local *lp = ctx->lp;
        struct at86rf215_chip_data *c = lp->data;
        bool force = false;
        ktime_t tim;
	u8 res;
        //printk (KERN_DEBUG "The function state_delay starts: SITUATION: from_state %d , to_state %d", ctx->from_state, ctx->to_state);
        switch (ctx->from_state) {
        case STATE_RF_NOP:
                switch (ctx->to_state) {
                case STATE_RF_TRXOFF:
        		//printk(KERN_DEBUG "t_reset_to_off : %d", c->t_reset_to_off);
	                tim = c->t_reset_to_off * NSEC_PER_USEC;
			//printk(KERN_DEBUG "TIM = %lld",tim);
                	goto change;
                default:
                        break;
                }
                break;
	/* To be continued, lol */
        default:
                break;
        }
        /* Default delay is 1us in the most cases */
        udelay(1);
	at86rf215_async_state_timer(&ctx->timer);
        return;
change: udelay (10);
	//printk (KERN_DEBUG "hrtimer_start");
        hrtimer_start(&ctx->timer, tim, HRTIMER_MODE_REL);
}

static void at86rf215_async_state_change_start(void *context)
{
        struct at86rf215_state_change *ctx = context;
        struct at86rf215_local *lp = ctx->lp;
        u8 *buf = ctx->buf;

        const u8 trx_state = buf[2] & TRX_STATE_MASK;
	//printk (KERN_DEBUG "The function 'async_state_change_start' starts");
        printk (KERN_DEBUG "SITUATION: from_state %d , to_state %d", ctx->from_state, ctx->to_state);
        //printk (KERN_DEBUG "BUF[2]=%hhx BUF[1]=%hhx BUF[0]=%hhx", buf[2], buf[1], buf[0]);
	/* Check for "possible" RF_TRANSITION_STATUS */
        if (trx_state == RF_TRANSITION_STATUS) {
                udelay(1);
                at86rf215_async_read_reg(lp, RG_RF09_CMD, ctx, at86rf215_async_state_change_start);
                return;
        }

        /* Check if we already are in the state which we change in */
        if (trx_state == ctx->to_state) {
                if (ctx->complete)
                        ctx->complete(context);
                return;
        }

        /* Set current state to the context of state change */
        ctx->from_state = trx_state;
        /* Going into the next step for a state change which do a timing
         * relevant delay.
         */
        at86rf215_async_write_reg(lp, RG_RF09_STATE, ctx->to_state, ctx, at86rf215_async_state_delay);
        //printk (KERN_DEBUG "BUF[2]:%x", ctx->buf[1]);

}


static void at86rf215_async_state_change(struct at86rf215_local *lp, struct at86rf215_state_change *ctx, const u8 state, void (*complete)(void *context))
{
        /* Initialization for the state change context */
	//printk(KERN_DEBUG "The function 'async_state_change' starts");
        ctx->to_state = state;
        ctx->complete = complete;
        at86rf215_async_read_reg(lp, RG_RF09_CMD, ctx, at86rf215_async_state_change_start);
}


static void at86rf215_sync_state_change_complete(void *context)
{
	//printk(KERN_DEBUG "The function 'sync_state_change_complete' starts");
        struct at86rf215_state_change *ctx = context;
        struct at86rf215_local *lp = ctx->lp;

        complete(&lp->state_complete);
}

/* This function do a sync framework above the async state change.
 * Some callbacks of the IEEE 802.15.4 driver interface need to behandled synchronously.
 */
static int at86rf215_sync_state_change(struct at86rf215_local *lp, unsigned int state)
{
	unsigned long rc;

	at86rf215_async_state_change(lp, &lp->state, state, at86rf215_sync_state_change_complete);
	//printk(KERN_DEBUG "WAIT_FOR_COMPLETION TIMEOUT starts now");
	rc = wait_for_completion_timeout(&lp->state_complete,msecs_to_jiffies(100)); // wait until we reach state_complete
        //printk(KERN_DEBUG "WAIT_FOR_COMPLETION TIMEOUT stops now");
	if (!rc) {
		at86rf215_async_error(lp, &lp->state, -ETIMEDOUT);
		return -ETIMEDOUT;
	}

	return 0;
}

/* The transmission is complete */
static void at86rf215_tx_complete(void *context)
{
        struct at86rf215_state_change *ctx = context;
        struct at86rf215_local *lp = ctx->lp;

        ieee802154_xmit_complete(lp->hw, lp->tx_skb, false);
        kfree(ctx);
}

// Transition from Transmission to reception : from state TX_ON to RX_AACK_ON
static void at86rf215_tx_on(void *context)
{
        struct at86rf215_state_change *ctx = context;
        struct at86rf215_local *lp = ctx->lp;

        at86rf215_async_state_change(lp, ctx, STATE_RF_RX, at86rf215_tx_complete);
}


// The following functions/structures will be dealt with/implemented later.
static void at86rf215_tx_trac_check(void *context){}
static void at86rf215_rx_read_frame_complete(void *context) {}
static void at86rf215_rx_trac_check(void *context) {}
static void at86rf215_irq_trx_end(void *context) {}
static void at86rf215_irq_status(void *context){}
static irqreturn_t at86rf215_isr(int irq, void *data){}

static void at86rf215_setup_spi_messages(struct at86rf215_local *lp, struct at86rf215_state_change *state)
{
	state->lp = lp;
	state->irq = lp->spi->irq;
	spi_message_init(&state->msg);
	state->msg.context = state;
	state->trx.len = 3; // 2 bytes for the address and 1 byte for the value read or written
	state->trx.tx_buf = state->buf;
	state->trx.rx_buf = state->buf;
	spi_message_add_tail(&state->trx, &state->msg);
	hrtimer_init(&state->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	state->timer.function = at86rf215_async_state_timer;
}


// Once this function is executed, we reach the "BUSY_TX" state.
static void at86rf215_write_frame_complete(void *context)
{
        struct at86rf215_state_change *ctx = context;
        struct at86rf215_local *lp = ctx->lp;

	//printk(KERN_DEBUG "The function write_frame_complete starts");
        ctx->trx.len = 2;

        if (gpio_is_valid(lp->slp_tr))
                at86rf215_slp_tr_rising_edge(lp);
        else
                at86rf215_async_write_reg(lp, RG_RF09_STATE, STATE_BUSY_TX, ctx, NULL); // ??
}

// Recieving the message from the MAC layer and save it in a buffer
static void at86rf215_write_frame(void *context)
{
        struct at86rf215_state_change *ctx = context;
        struct at86rf215_local *lp = ctx->lp;
        struct sk_buff *skb = lp->tx_skb;
        u8 *buf = ctx->buf;
        int rc;

        lp->is_tx = 1;

        buf[0] = CMD_FB | CMD_WRITE;  // ??
        buf[1] = skb->len + 2;
        memcpy(buf + 2, skb->data, skb->len); // copy skb->data to buf .
        ctx->trx.len = skb->len + 2;
        ctx->msg.complete = at86rf215_write_frame_complete;
        rc = spi_async(lp->spi, &ctx->msg);
        if (rc) {
		//printk(KERN_DEBUG "spi_async failed in write_frame.");
                ctx->trx.len = 2; //  skb->len = 0 ==> skb vide
                at86rf215_async_error(lp, ctx, rc);
        }
}

//Change from state TX_ON to state TX_ARET_ON
static void at86rf215_xmit_tx_on(void *context){}

// Both functions used to determine transitions between the following states : STATE_TX_ARET_ON, STATE_TX_ON, STATE_TRX_OFF.
static void at86rf215_xmit_start(void *context){}
static int at86rf215_xmit(struct ieee802154_hw *hw, struct sk_buff *skb)
{
	return 0;
}

// ??
static int at86rf215_ed(struct ieee802154_hw *hw, u8 *level)
{
        WARN_ON(!level);
        *level = 0xbe;
        return 0;
}

// Both followinf functions could be deleted later.
// Start reception with ACK ON
static int at86rf215_start(struct ieee802154_hw *hw)
{
        struct at86rf215_local *lp = hw->priv;

        /* reset trac stats on start */
        if (IS_ENABLED(CONFIG_IEEE802154_AT86RF230_DEBUGFS))
                memset(&lp->trac, 0, sizeof(struct at86rf215_trac));

        at86rf215_awake(lp);
        enable_irq(lp->spi->irq);

        return at86rf215_sync_state_change(lp, STATE_RF_RX);
}

//Stop reception ( with CSMA )
static void at86rf215_stop(struct ieee802154_hw *hw)
{
        struct at86rf215_local *lp = hw->priv;
        u8 csma_seed[2];

        at86rf215_sync_state_change(lp, STATE_FORCE_TRX_OFF); // ??

        disable_irq(lp->spi->irq);

        /* It's recommended to set random new csma_seeds before sleep state.
         * Makes only sense in the stop callback, not doing this inside of
         * at86rf230_sleep, this is also used when we don't transmit afterwards
         * when calling start callback again.
         */
        get_random_bytes(csma_seed, ARRAY_SIZE(csma_seed));
        at86rf215_write_subreg(lp, SR_CSMA_SEED_0, csma_seed[0]);// ??
        at86rf215_write_subreg(lp, SR_CSMA_SEED_1, csma_seed[1]);// ??

        at86rf215_sleep(lp);
}

// Change channel : The state must be TRXOFF || The function shouldnt be "write_subreg"
static int at86rf215_set_channel(struct at86rf215_local *lp, u8 page, u8 channel)
{
//        return at86rf215_write_subreg(lp, RG_RF09_CS, channel);
	channel = channel & 0xff;
	return __at86rf215_write(lp, RG_RF09_CS, channel);
}

#define AT86RF215_MAX_ED_LEVELS 0xF // 16 registers
static const s32 at86rf215_ed_levels[AT86RF215_MAX_ED_LEVELS + 1] = { // [WARNING] : These values are so random, for now.
        -9800, -9600, -9400, -9200, -9000, -8800, -8600, -8400, -8200, -8000, -7800, -7600, -7400, -7200, -7000, -6800 };

/* This function set the channel and the page +  set the timeout to recalibrate | Jiffies : Kernel Internal value
 * c.a.d, à partir de ce moment, qu'on comptera 5 minutes avant la recalibration.
 * WHY : + 10 ?
 */
static int at86rf215_channel(struct ieee802154_hw *hw, u8 page, u8 channel)
{
        struct at86rf215_local *lp = hw->priv;
        int rc;

        rc = lp->data->set_channel(lp, page, channel);
        /* Wait for PLL */
        usleep_range(lp->data->t_channel_switch,
                     lp->data->t_channel_switch + 10);

        lp->cal_timeout = jiffies + AT86RF215_CAL_LOOP_TIMEOUT;
        return rc;
}

/* This function check if there are registers values that changed. */
/**************** To complete ******************/
static int at86rf215_set_hw_addr_filt(struct ieee802154_hw *hw, struct ieee802154_hw_addr_filt *filt, unsigned long changed)
{
	return 0;
}

#define AT86RF215_MAX_TX_POWERS 0x1F // 32 registres
static const s32 at86rf215_powers[AT86RF215_MAX_TX_POWERS + 1] = { 3100, 3000, 2900, 2800, 2700, 2600, 2500, 2400, 2300,
2200, 2100, 2000, 1900, 1800, 1700, 1600, 1500, 1400, 1300, 1200, 1100, 1000, 900, 800, 700, 600, 500, 400, 300, 200, 100,0}; // page 50 , register concerné : TXPWR

// change  the corresponding register with the suitable power value
static int at86rf2xx_set_txpower(struct at86rf215_local *lp, s32 mbm)
{
        u32 i;

        for (i = 0; i < lp->hw->phy->supported.tx_powers_size; i++) {
                if (lp->hw->phy->supported.tx_powers[i] == mbm)
                        return at86rf215_write_subreg(lp, SR_RF09_PAC_TXPWR, i);
        }

        return -EINVAL;
}

//Set the transmitter power
static int at86rf215_set_txpower(struct ieee802154_hw *hw, s32 mbm)
{
        struct at86rf215_local *lp = hw->priv;

        return lp->data->set_txpower(lp, mbm);
}

//LBT: Listen Before transmitting ==> CSMA/CA
static int at86rf215_set_lbt(struct ieee802154_hw *hw, bool on)
{
        struct at86rf215_local *lp = hw->priv;

       return at86rf215_write_subreg(lp, SR_CSMA_LBT_MODE, on); // ??
}


// The following function set the CCA mode || This function will be deleted later ( cause we only have one CCA mode )
static int at86rf215_set_cca_mode(struct ieee802154_hw *hw, const struct wpan_phy_cca *cca)
{
        struct at86rf215_local *lp = hw->priv;
        u8 val;

        /* mapping 802.15.4 to driver spec */
        switch (cca->mode) {
        case NL802154_CCA_ENERGY:
                val = 1;
                break;
        case NL802154_CCA_CARRIER:
                val = 2;
                break;
        case NL802154_CCA_ENERGY_CARRIER:
                switch (cca->opt) {
                case NL802154_CCA_OPT_ENERGY_CARRIER_AND:
                        val = 3;
                        break;
                case NL802154_CCA_OPT_ENERGY_CARRIER_OR:
                        val = 0;
                        break;
                default:
                        return -EINVAL;
                }
                break;
        default:
                return -EINVAL;
        }

        return at86rf215_write_subreg(lp, SR_CCA_MODE, val);
}


/* The following function search for the CCA level set ( in the function parameters ) if it is supported.
 * Then affected to the registre SR_CCA-ED_THRES.
 * PS : CCA shall report a busy medium upon detecting any energy above the ED threshold.
 */
static int at86rf215_set_cca_ed_level(struct ieee802154_hw *hw, s32 mbm)
{
        struct at86rf215_local *lp = hw->priv;
        u32 i;

        for (i = 0; i < hw->phy->supported.cca_ed_levels_size; i++) {
                if (hw->phy->supported.cca_ed_levels[i] == mbm)
//                        return at86rf215_write_subreg(lp, RG_BBC0_AMEDT, i);
			return __at86rf215_write(lp, RG_BBC0_AMEDT, i);
        }

        return -EINVAL;
}



/* CSMA is caracterized by 4 parameters : SR_MIN_BE, SR_MAX_BE, SR_MAX_CSMA_RETRIES, SR_MAX_FRAME_RETRIES.
 * NOTE : The following functions could be deleted later, both.
 */ //????????????

// Set parameters for CSMA-CA (PART 1)
static int at86rf215_set_csma_params(struct ieee802154_hw *hw, u8 min_be, u8 max_be,u8 retries)
{
        struct at86rf215_local *lp = hw->priv;
        int rc;

        rc = at86rf215_write_subreg(lp, SR_MIN_BE, min_be);
        if (rc)
                return rc;

        rc = at86rf215_write_subreg(lp, SR_MAX_BE, max_be);
        if (rc)
                return rc;

        return at86rf215_write_subreg(lp, SR_MAX_CSMA_RETRIES, retries);
}

// Set parameters for CSMA-CA (PART 2)
static int at86rf215_set_frame_retries(struct ieee802154_hw *hw, s8 retries)
{
        struct at86rf215_local *lp = hw->priv;

        return at86rf215_write_subreg(lp, SR_MAX_FRAME_RETRIES, retries);
}


/* Promiscuous mode :
 * The Automatic Acknowledgement shoud be disabled ( resp. enabled) and
 * the promiscuous mode shoud be enabled ( resp. disabled).
 */
static int at86rf215_set_promiscuous_mode(struct ieee802154_hw *hw, const bool on)
{
        struct at86rf215_local *lp = hw->priv;
        int rc;

        if (on) {
                rc = at86rf215_write_subreg(lp, SR_BBC0_AMCS_AACK, 1);
                if (rc < 0)
                        return rc;

                rc = at86rf215_write_subreg(lp, SR_BBC0_AFC0_PM, 1);
                if (rc < 0)
                        return rc;
        } else {
                rc = at86rf215_write_subreg(lp, SR_BBC0_AMCS_AACK, 0);
                if (rc < 0)
                        return rc;

                rc = at86rf215_write_subreg(lp, SR_BBC0_AFC0_PM, 0);
                if (rc < 0)
                        return rc;
        }

        return 0;
}

static const struct ieee802154_ops at86rf215_ops = {
        .owner = THIS_MODULE,
        .xmit_async = at86rf215_xmit,
        .ed = at86rf215_ed,
        .set_channel = at86rf215_channel,
        .start = at86rf215_start,
        .stop = at86rf215_stop,
        .set_hw_addr_filt = at86rf215_set_hw_addr_filt,
        .set_txpower = at86rf215_set_txpower,
        .set_lbt = at86rf215_set_lbt,
        .set_cca_mode = at86rf215_set_cca_mode,
        .set_cca_ed_level = at86rf215_set_cca_ed_level,
        .set_csma_params = at86rf215_set_csma_params,
        .set_frame_retries = at86rf215_set_frame_retries,
        .set_promiscuous_mode = at86rf215_set_promiscuous_mode,

};


// Datasheet : page 189 (Transition time)
// There more transition times that maybe will be added later.
static struct at86rf215_chip_data at86rf215_data = {
        .t_sleep_cycle = 500, // time after leaving P_ON state (us) ==> to clk signal available ( concerned time : tDEEP_SLEEP_TRXOFF )
        .t_channel_switch = 100, // Duration of channel switch within frequency band. I chose the MAX. ( concerned time : tPLL_CH_SW )
        .t_reset_to_off = 1, // tRESET_TRXOFF
        .t_off_to_aack = 90, // tTRXOFF_TXPREP
        .t_off_to_tx_on = 90, // tTRXOFF_RX
        //.t_off_to_sleep = ?, // Apparament, sa valeur n'existe pas.
        .t_sleep_to_off = 1, // tSLEEP_TRXOFF
        .t_frame = 4096,  //?
        .t_p_ack = 545,
        .rssi_base_val = -117,
        .set_channel = at86rf215_set_channel,
        .set_txpower = at86rf2xx_set_txpower,
};


static int at86rf215_hw_init(struct at86rf215_local *lp, u8 xtal_trim)
{
	int rc, irq_type, irq_pol = IRQ_ACTIVE_HIGH; // Par choix, elle peut etre configurée en IRQ_ACTIVE_LOW
	unsigned int dvdd;
	u8 csma_seed[2];
	unsigned int c, data;
	unsigned int coucou;

	rc = at86rf215_sync_state_change(lp, STATE_RF_TRXOFF); // Initial state in the state machine graph
//	if (rc) {
//		printk(KERN_DEBUG "Initial state : FAILED!");
		return rc;
//	}
/*
	irq_type = irq_get_trigger_type(lp->spi->irq);
	if (irq_type == IRQ_TYPE_EDGE_FALLING || irq_type == IRQ_TYPE_LEVEL_LOW)
		irq_pol = IRQ_ACTIVE_LOW;
	rc = at86rf215_write_subreg(lp, SR_RF_CFG_IRQP, irq_pol);
	if (rc)
		return rc;
/* I don't know what this does */
/*	rc = at86rf215_write_subreg(lp, SR_RX_SAFE_MODE, 1);
	if (rc)
		return rc;
*/
/*	rc = at86rf215_write_subreg(lp, SG_BBC0_IRQM, IRQS_RXFE);
	if (rc)
		return rc;
	rc = at86rf215_write_subreg(lp, SG_BBC0_IRQM, IRQS_TXFE);
        if (rc)
                return rc;
*/
/* I don't know what this does */
/*	// reset values differs in at86rf231 and at86rf233
	rc = at86rf230_write_subreg(lp, SR_IRQ_MASK_MODE, 0);
	if (rc)
		return rc;
*/
/*
	get_random_bytes(csma_seed, ARRAY_SIZE(csma_seed));
	rc = at86rf230_write_subreg(lp, SR_CSMA_SEED_0, csma_seed[0]);
	if (rc)
		return rc;
	rc = at86rf230_write_subreg(lp, SR_CSMA_SEED_1, csma_seed[1]);
	if (rc)
		return rc;
	// CLKM changes are applied immediately 
	rc = at86rf230_write_subreg(lp, SR_CLKM_SHA_SEL, 0x00);
	if (rc)
		return rc;
	// Turn CLKM Off 
	rc = at86rf230_write_subreg(lp, SR_CLKM_CTRL, 0x00);
	if (rc)
		return rc;
	// Wait the next SLEEP cycle 
	usleep_range(lp->data->t_sleep_cycle,
		     lp->data->t_sleep_cycle + 100);
*/
	/* xtal_trim value is calculated by:
	 * CL = 0.5 * (CX + CTRIM + CPAR)
	 *
	 * whereas:
	 * CL = capacitor of used crystal
	 * CX = connected capacitors at xtal pins
	 * CPAR = in all at86rf2xx datasheets this is a constant value 3 pF,
	 *	  but this is different on each board setup. You need to fine
	 *	  tuning this value via CTRIM.
	 * CTRIM = variable capacitor setting. Resolution is 0.3 pF range is
	 *	   0 pF upto 4.5 pF.
	 *
	 * Examples:
	 * atben transceiver:
	 *
	 * CL = 8 pF
	 * CX = 12 pF
	 * CPAR = 3 pF (We assume the magic constant from datasheet)
	 * CTRIM = 0.9 pF
	 *
	 * (12+0.9+3)/2 = 7.95 which is nearly at 8 pF
	 *
	 * xtal_trim = 0x3
	 *
	 * openlabs transceiver:
	 *
	 * CL = 16 pF
	 * CX = 22 pF
	 * CPAR = 3 pF (We assume the magic constant from datasheet)
	 * CTRIM = 4.5 pF
	 *
	 * (22+4.5+3)/2 = 14.75 which is the nearest value to 16 pF
	 *
	 * xtal_trim = 0xf
	 */
/*	rc = at86rf230_write_subreg(lp, SR_XTAL_TRIM, xtal_trim);
	if (rc)
		return rc;
	rc = at86rf230_read_subreg(lp, SR_DVDD_OK, &dvdd);
	if (rc)
		return rc;
	if (!dvdd) {
		dev_err(&lp->spi->dev, "DVDD error\n");
		return -EINVAL;
	}
*/
	/* Force setting slotted operation bit to 0. Sometimes the atben
	 * sets this bit and I don't know why. We set this always force
	 * to zero while probing.
	 */
//	return at86rf230_write_subreg(lp, SR_SLOTTED_OPERATION, 0);
}

//Check if device tree definition for the spi device is correct.
static int at86rf215_get_pdata(struct spi_device *spi, int *rstn, int *slp_tr, u8 *xtal_trim)
{
        struct at86rf215_platform_data *pdata = spi->dev.platform_data; 
        int ret;

        if (!IS_ENABLED(CONFIG_OF) || !spi->dev.of_node) { // dev.of_node :associated device tree node 
                if (!pdata)
                        return -ENOENT;

                *rstn = pdata->rstn;
                *slp_tr = pdata->slp_tr;
                *xtal_trim = pdata->xtal_trim;
                return 0;
        }

        *rstn = of_get_named_gpio(spi->dev.of_node, "reset-gpio", 0); // Get the GPIO Number
        *slp_tr = of_get_named_gpio(spi->dev.of_node, "sleep-gpio", 0); // Get the GPIO Number
        ret = of_property_read_u8(spi->dev.of_node, "xtal-trim", xtal_trim); // search  for the property in a node (from the device tree) and read 8-bit value(s) from it.
        if (ret < 0 && ret != -EINVAL)
                return ret;
        return 0;
}


static int at86rf215_detect_device(struct at86rf215_local *lp)
{
        unsigned int part, version;
        const char *chip;
        int rc;

        pr_info("[Detecting]: detect_device function is being called ..\n");

        rc = __at86rf215_read(lp, RG_RF_VN, &version);
        if (rc)
                return rc;
        //printk(KERN_DEBUG "[Detecting]: AT86RF215 version: %x", version);

        rc = __at86rf215_read(lp, RG_RF_PN, &part);
        if (rc)
                return rc;
        //printk (KERN_DEBUG "[Detecting]: AT86RF215 part number: %x", part);

/*
        if ( (version != 1) || (version != 3)) {
                dev_err(&lp->spi->dev, "Version Number doesn't exist ( version : %x)\n", version);
                return -EINVAL;
        }
        if ((part != 52) | (part != 53) || (part != 54)) {
                dev_err(&lp->spi->dev, "Part Number doesn't exist ( Part Number : %x)\n", part);
                return -EINVAL;
        }
*/

//mac802154.h
        lp->hw->flags = IEEE802154_HW_TX_OMIT_CKSUM | //Indicates that xmitter will add FCS automatically
			IEEE802154_HW_RX_OMIT_CKSUM | //Indicates that transmitter will add FCS automatically
                        IEEE802154_HW_CSMA_PARAMS | //Indicates that transceiver will support csma parameters (max_be, min_be, backoff exponents). (page 172 )
                        IEEE802154_HW_FRAME_RETRIES | //Indicates that transceiver will support ARET frame retries setting.
			IEEE802154_HW_AFILT | //Indicates that transceiver will support hardware address filter setting. / ????
                        IEEE802154_HW_PROMISCUOUS; //Indicates that transceiver will support promiscuous mode setting.
//cfg802154.h
        lp->hw->phy->flags = WPAN_PHY_FLAG_TXPOWER | //Indicates that transceiver will support transmit power setting. ( La puissance du signal envoyé) Supports ??
                             WPAN_PHY_FLAG_CCA_ED_LEVEL | //Indicates that transceiver will support cca ed level setting. ( The ED threshold )
                             WPAN_PHY_FLAG_CCA_MODE; //Indicates that transceiver will support cca mode setting. 

// NL802154_CCA_ENERGY :Energy above threshold
	lp->hw->phy->supported.cca_modes = BIT(NL802154_CCA_ENERGY); //" (CCA-ED) is supported only" , Datasheet : page 148

        lp->hw->phy->supported.cca_opts = BIT(NL802154_CCA_OPT_ENERGY_CARRIER_AND) | BIT(NL802154_CCA_OPT_ENERGY_CARRIER_OR);

        lp->hw->phy->cca.mode = NL802154_CCA_ENERGY;

        lp->data = &at86rf215_data;
//?     lp->hw->phy->supported.channels[0] = 0x00007FF;
//?     lp->hw->phy->supported.channels[2] = 0x00007FF;
//?     lp->hw->phy->current_channel = 5;
        lp->hw->phy->symbol_duration = 4; //at86rf215 (ttx_start_delay)  , at86rf230 (tTR10 )  /C’est la durée d’un symbole PSDU modulé et codé/ ==>  TO CHECK AGAIN
//?        lp->hw->phy->supported.lbt = NL802154_SUPPORTED_BOOL_BOTH; //bool states for bool capability entry ? both true and false. ??
        lp->hw->phy->supported.tx_powers = at86rf215_powers;
        lp->hw->phy->supported.tx_powers_size = ARRAY_SIZE(at86rf215_powers);
        lp->hw->phy->supported.cca_ed_levels = at86rf215_ed_levels;
        lp->hw->phy->supported.cca_ed_levels_size = ARRAY_SIZE(at86rf215_ed_levels);

//Define the ED threshold + the transmitter power
        lp->hw->phy->cca_ed_level = lp->hw->phy->supported.cca_ed_levels[7]; //laquelle choisir ?
        lp->hw->phy->transmit_power = lp->hw->phy->supported.tx_powers[0]; //laquelle choisir ?

	dev_info(&lp->spi->dev, "[Detecting]: Detected At86rf215 chip version %d\n", version);

	return rc;
}

/************************************************************************************** DEBUGGING ****************************************************************************************/
#ifdef CONFIG_IEEE802154_AT86RF215_DEBUGFS
static struct dentry *at86rf215_debugfs_root;

static int at86rf215_stats_show(struct seq_file *file, void *offset)
{
	struct at86rf215_local *lp = file->private;

	seq_printf(file, "SUCCESS:\t\t%8llu\n", lp->trac.success);
	seq_printf(file, "SUCCESS_DATA_PENDING:\t%8llu\n",
		   lp->trac.success_data_pending);
	seq_printf(file, "SUCCESS_WAIT_FOR_ACK:\t%8llu\n",
		   lp->trac.success_wait_for_ack);
	seq_printf(file, "CHANNEL_ACCESS_FAILURE:\t%8llu\n",
		   lp->trac.channel_access_failure);
	seq_printf(file, "NO_ACK:\t\t\t%8llu\n", lp->trac.no_ack);
	seq_printf(file, "INVALID:\t\t%8llu\n", lp->trac.invalid);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(at86rf215_stats);

static int at86rf215_debugfs_init(struct at86rf215_local *lp)
{
	char debugfs_dir_name[DNAME_INLINE_LEN + 1] = "at86rf215";
	struct dentry *stats;

	strncat(debugfs_dir_name, dev_name(&lp->spi->dev), DNAME_INLINE_LEN);

	at86rf215_debugfs_root = debugfs_create_dir(debugfs_dir_name, NULL);
	if (!at86rf215_debugfs_root)
		return -ENOMEM;

	stats = debugfs_create_file("trac_stats", 0444,at866rf215_debugfs_root, lp,at86rf215_stats_fops);
	if (!stats)
		return -ENOMEM;

	return 0;
}

static void at86rf215_debugfs_remove(void)
{
	debugfs_remove_recursive(at86rf215_debugfs_root);
}

#else
static int at86rf215_debugfs_init(struct at86rf215_local *lp) { return 0; }
static void at86rf215_debugfs_remove(void) { }
#endif
/********************************************************************************** END OF DEBUGGING ************************************************************************************/


static int at86rf215_probe(struct spi_device *spi)
{
	struct ieee802154_hw *hw; //IEEE 802.15.4 hardware device
        struct at86rf215_local *lp;
        unsigned int status, stat;
        int rc, irq_type, rstn, slp_tr,err;
        u8 xtal_trim = 0;
        pr_info("[Probing]: AT86RF215 probe function is called ..\n");

        if (!spi->irq) {
                dev_err(&spi->dev, "no IRQ specified\n");
                return -EINVAL;
        }

	pr_info("[Probing]: Checking whether device tree is well configured ..\n");
        rc = at86rf215_get_pdata(spi, &rstn, &slp_tr, &xtal_trim);
        if (rc < 0) {
                dev_err(&spi->dev, "failed to parse platform_data : %d .\n Please check your device tree. ", rc);
                return rc;
        }

        pr_info("[Probing]: Checking whether gpio PINs configured in the device tree could be used ..\n");
/*
        if (gpio_is_valid(rstn)) { // This function turns 0 if gpio is valid
		// request a single GPIO with initial setup IF NOT
                pr_info("[Probing]: requesting a RESET PIN for GPIO ..");
                rc = devm_gpio_request_one(&spi->dev, rstn, GPIOF_OUT_INIT_HIGH, "rstn");
                if (rc){
                        return rc;
			}
        }
        if (gpio_is_valid(slp_tr)) {
		// request a single GPIO with initial setup IF NOT
                rc = devm_gpio_request_one(&spi->dev, slp_tr, GPIOF_OUT_INIT_LOW, "slp_tr");
		pr_info("[Probing]: requesting a SLEEP PIN for GPIO ..");
                if (rc){
                        return rc;
                        }
        }
        if (gpio_is_valid(rstn)) {
	        pr_info("[Probing]: The board is being reset.");
                udelay(1);
                gpio_set_value_cansleep(rstn, 0);
                udelay(1);
                gpio_set_value_cansleep(rstn, 1);
                usleep_range(120, 240);
        }
        hw = ieee802154_alloc_hw(sizeof(*lp), &at86rf215_ops); //Allocate memory for a new hardware device. This must be called once for each hardware device.
        if (!hw){
		//printk(KERN_ALERT "[Probing]: The hardware couldn't be allocated: %d", ENOMEM); // Out Of memory
                return -ENOMEM;
		}
        lp = hw->priv;
        lp->hw = hw;
        lp->spi = spi;
        lp->slp_tr = slp_tr;
        hw->parent = &spi->dev;
        ieee802154_random_extended_addr(&hw->phy->perm_extended_addr); // why would he request an extended address ( sur 8 octets ) ? We'll see LATER.
        lp->regmap = devm_regmap_init_spi(spi, &at86rf215_regmap_spi_config); // This function define SPI Protocol specifications.
        if (IS_ERR(lp->regmap)) {
                rc = PTR_ERR(lp->regmap);
                dev_err(&spi->dev, "[Probing]: Failed to allocate register map: %d\n", rc);
                goto free_dev;
        }
        at86rf215_setup_spi_messages(lp, &lp->state);
        at86rf215_setup_spi_messages(lp, &lp->tx);
        rc = at86rf215_detect_device(lp);
        if (rc < 0)
                goto free_dev;
        init_completion(&lp->state_complete); //init_completion - Initialize a dynamically allocated completion pointer "lp->state_complete" to completion structure that is to be initialized
        spi_set_drvdata(spi, lp); // spi->dev->driver_data = lp
	rc = at86rf215_hw_init(lp, xtal_trim);
        err=__at86rf215_read(lp, RG_RF09_CMD, &stat);
        //printk (KERN_DEBUG "RG_RF09_CMD = %x", stat);
	if (rc) {
		printk (KERN_DEBUG "The function hw_init faaaaaaaaaaaaaaaaaaaaaaaaailed");
		goto free_dev;
	}
/*        // Read irq status register to reset irq line
        rc = at86rf215_read_subreg(lp, RG_IRQ_STATUS, 0xff, 0, &status);
        if (rc)
                goto free_dev;
        irq_type = irq_get_trigger_type(spi->irq);
        if (!irq_type)
                irq_type = IRQF_TRIGGER_HIGH;
        rc = devm_request_irq(&spi->dev, spi->irq, at86rf215_isr,IRQF_SHARED | irq_type, dev_name(&spi->dev), lp);
        if (rc)
                goto free_dev;
        // disable_irq by default and wait for starting hardware
        disable_irq(spi->irq);
        // going into sleep by default
        at86rf215_sleep(lp);
        rc = at86rf215_debugfs_init(lp);
        if (rc)
                goto free_dev;
        rc = ieee802154_register_hw(lp->hw);
        if (rc)
                goto free_debugfs;
        return rc;
*/
free_debugfs:
        at86rf215_debugfs_remove();

free_dev:
        ieee802154_free_hw(lp->hw);

	return 0;
}

static int at86rf215_remove(struct spi_device *spi)
{
	pr_info("[Removing]: AT86RF215 remove function is called ..\n");
        return 0;
}

static const struct of_device_id at86rf215_of_match[] = {
        { .compatible = "atmel,at86rf215", },
        { },
};
MODULE_DEVICE_TABLE(of, at86rf215_of_match);


static const struct spi_device_id at86rf215_device_id[] = {
        { .name = "at86rf215", },
        { },
};
MODULE_DEVICE_TABLE(spi, at86rf215_device_id);

//Faisant appel à la structure "spi_driver" sans spi.h
static struct spi_driver at86rf215_driver = {
        .id_table = at86rf215_device_id, // List of SPI devices supported by this drive
        .driver = { 
	        .of_match_table = of_match_ptr(at86rf215_of_match), // of_match_ptr :  Find and read an array of u8 from a property. // of_match_table : The open firmware table
        	.name   = "at86rf215",
        },
        .probe      = at86rf215_probe,
        .remove     = at86rf215_remove,
};

module_spi_driver(at86rf215_driver);

MODULE_DESCRIPTION("AT86RF215 Transceiver Driver");
MODULE_LICENSE("GPL v2");



