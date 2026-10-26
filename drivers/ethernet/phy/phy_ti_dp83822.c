/*
 * Copyright (c) 2026 Calixto Systems Pvt Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Driver for the Texas Instruments DP83822 10/100 Mbps Ethernet PHY.
 *
 * Reference: DP83822 datasheet (SNLS505G, Rev G, August 2023)
 *            Texas Instruments, https://www.ti.com/lit/ds/symlink/dp83822i.pdf
 */

#define DT_DRV_COMPAT ti_dp83822

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/phy.h>
#include <zephyr/net/mii.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(phy_ti_dp83822, CONFIG_PHY_LOG_LEVEL);

/* -----------------------------------------------------------------------
 * Register definitions
 * ----------------------------------------------------------------------- */

/* Vendor-specific registers (Clause 22 direct space) */
#define DP83822_PHYSTS_REG          0x10
#define DP83822_RCSR_REG            0x17
#define DP83822_PHYRCR_REG          0x1F

/* Indirect (extended) register access, Clause 22 + MMD (IEEE 802.3 22.2.4.3.11) */
#define DP83822_REGCR_REG           0x0D
#define DP83822_ADDAR_REG           0x0E
#define DP83822_REGCR_ADDR          0x001F /* function = address, DEVAD = 31   */
#define DP83822_REGCR_DATA_NOINC    0x401F /* function = data, no post-incr.   */

/* Extended registers (reached through REGCR/ADDAR) */
#define DP83822_SOR1_REG            0x0467

/* PHYSTS (0x10) */
#define DP83822_PHYSTS_LINK_STATUS  BIT(0)
#define DP83822_PHYSTS_SPEED_10     BIT(1)
#define DP83822_PHYSTS_DUPLEX       BIT(2)
#define DP83822_PHYSTS_AUTONEG_DONE BIT(4)

/* RCSR (0x17) */
#define DP83822_RCSR_RMII_MODE_EN   BIT(5)
#define DP83822_RCSR_RMII_MODE_SEL  BIT(7)

/* PHYRCR (0x1F) */
#define DP83822_PHYRCR_SW_RESET     BIT(15)
#define DP83822_PHYRCR_DIG_RESTART  BIT(14)

/* PHY identifier (family mask) */
#define DP83822_PHY_ID              0x2000A240U
#define DP83822_PHY_ID_MASK         0xFFFFFFF0U

#define DP83822_SW_RESET_TIMEOUT_MS 500U
#define DP83822_SW_RESET_POLL_MS    1U
#define DP83822_POST_RESET_DELAY_US 2500U

enum dp83822_rmii_mode {
	DP83822_RMII_MASTER = 0,
	DP83822_RMII_SLAVE  = 1,
};

struct dp83822_config {
	uint8_t addr;
	const struct device *mdio_dev;
	enum dp83822_rmii_mode rmii_mode;
};

struct dp83822_data {
	const struct device *dev;
	struct phy_link_state state;
	phy_callback_t cb;
	void *cb_data;
	struct k_mutex mutex;
	struct k_work_delayable phy_monitor_work;

	/* Last configuration passed to cfg_link(), used for -EALREADY */
	enum phy_link_speed cfg_speeds;
	enum phy_cfg_link_flag cfg_flags;
	bool cfg_valid;
};

/* -----------------------------------------------------------------------
 * Register access
 * ----------------------------------------------------------------------- */
static int phy_dp83822_read(const struct device *dev,
			    uint16_t reg_addr, uint32_t *data)
{
	const struct dp83822_config *config = dev->config;
	uint16_t val = 0U;
	int ret;

	ret = mdio_read(config->mdio_dev, config->addr, reg_addr, &val);
	if (ret) {
		return ret;
	}

	*data = val;
	return 0;
}

static int phy_dp83822_write(const struct device *dev,
			     uint16_t reg_addr, uint32_t data)
{
	const struct dp83822_config *config = dev->config;

	return mdio_write(config->mdio_dev, config->addr, reg_addr, (uint16_t)data);
}

/* Extended register access via REGCR/ADDAR (registers above 0x1F). */
static int phy_dp83822_ext_read(const struct device *dev,
				uint16_t reg_addr, uint32_t *data)
{
	int ret;

	ret = phy_dp83822_write(dev, DP83822_REGCR_REG, DP83822_REGCR_ADDR);
	if (ret) {
		return ret;
	}
	ret = phy_dp83822_write(dev, DP83822_ADDAR_REG, reg_addr);
	if (ret) {
		return ret;
	}
	ret = phy_dp83822_write(dev, DP83822_REGCR_REG, DP83822_REGCR_DATA_NOINC);
	if (ret) {
		return ret;
	}
	return phy_dp83822_read(dev, DP83822_ADDAR_REG, data);
}

static int phy_dp83822_ext_write(const struct device *dev,
				 uint16_t reg_addr, uint32_t data)
{
	int ret;

	ret = phy_dp83822_write(dev, DP83822_REGCR_REG, DP83822_REGCR_ADDR);
	if (ret) {
		return ret;
	}
	ret = phy_dp83822_write(dev, DP83822_ADDAR_REG, reg_addr);
	if (ret) {
		return ret;
	}
	ret = phy_dp83822_write(dev, DP83822_REGCR_REG, DP83822_REGCR_DATA_NOINC);
	if (ret) {
		return ret;
	}
	return phy_dp83822_write(dev, DP83822_ADDAR_REG, data);
}

/* -----------------------------------------------------------------------
 * Auto-negotiation
 * ----------------------------------------------------------------------- */
static int phy_dp83822_autonegotiate(const struct device *dev)
{
	const struct dp83822_config *config = dev->config;
	uint32_t bmcr = 0;
	uint32_t bmsr = 0;
	uint16_t timeout = CONFIG_PHY_AUTONEG_TIMEOUT_MS / 100;
	int ret;

	ret = phy_dp83822_read(dev, MII_BMCR, &bmcr);
	if (ret) {
		LOG_ERR("PHY (%d) BMCR read failed: %d", config->addr, ret);
		return ret;
	}

	LOG_DBG("PHY (%d) starting auto-negotiation", config->addr);
	bmcr |= MII_BMCR_AUTONEG_ENABLE | MII_BMCR_AUTONEG_RESTART;
	bmcr &= ~MII_BMCR_ISOLATE;

	ret = phy_dp83822_write(dev, MII_BMCR, bmcr);
	if (ret) {
		LOG_ERR("PHY (%d) BMCR write failed: %d", config->addr, ret);
		return ret;
	}

	do {
		if (timeout-- == 0) {
			LOG_DBG("PHY (%d) auto-negotiation timed out", config->addr);
			/* -ENETDOWN: link timeout, not an MDIO communication error */
			return -ENETDOWN;
		}
		k_msleep(100);

		ret = phy_dp83822_read(dev, MII_BMSR, &bmsr);
		if (ret) {
			LOG_ERR("PHY (%d) BMSR read failed: %d", config->addr, ret);
			return ret;
		}
	} while (!(bmsr & MII_BMSR_AUTONEG_COMPLETE));

	LOG_DBG("PHY (%d) auto-negotiation complete", config->addr);
	return 0;
}

/* Force speed/duplex with auto-negotiation disabled. */
static int phy_dp83822_force_link(const struct device *dev,
				  enum phy_link_speed speeds)
{
	uint32_t bmcr = 0;
	int ret;

	ret = phy_dp83822_read(dev, MII_BMCR, &bmcr);
	if (ret) {
		return ret;
	}

	bmcr &= ~(MII_BMCR_AUTONEG_ENABLE | MII_BMCR_SPEED_100 |
		  MII_BMCR_DUPLEX_MODE | MII_BMCR_ISOLATE);

	if (PHY_LINK_IS_SPEED_100M(speeds)) {
		bmcr |= MII_BMCR_SPEED_100;
	}
	if (PHY_LINK_IS_FULL_DUPLEX(speeds)) {
		bmcr |= MII_BMCR_DUPLEX_MODE;
	}

	return phy_dp83822_write(dev, MII_BMCR, bmcr);
}

/* -----------------------------------------------------------------------
 * Link state
 * ----------------------------------------------------------------------- */
static int phy_dp83822_get_link(const struct device *dev,
				struct phy_link_state *state)
{
	const struct dp83822_config *config = dev->config;
	struct dp83822_data *data = dev->data;
	struct phy_link_state old_state = data->state;
	uint32_t bmsr = 0;
	uint32_t bmcr = 0;
	uint32_t anar = 0;
	uint32_t anlpar = 0;
	int ret;

	static uint8_t pr_counter;

	ret = k_mutex_lock(&data->mutex, K_FOREVER);
	if (ret) {
		LOG_ERR("PHY mutex lock error");
		return ret;
	}

	ret = phy_dp83822_read(dev, MII_BMSR, &bmsr);
	if (ret) {
		LOG_ERR("Error reading phy (%d) basic status register", config->addr);
		goto unlock;
	}
	state->is_up = bmsr & MII_BMSR_LINK_STATUS;

	if (!state->is_up) {
		/* BMSR link bit is latching-low: a momentary blip since the last
		 * read shows as "down". Cross-check against PHYSTS, which is
		 * real-time, so a latched blip is not reported as a link loss.
		 */
		uint32_t bmsr2 = 0;
		uint32_t physts = 0;

		(void)phy_dp83822_read(dev, MII_BMSR, &bmsr2);
		(void)phy_dp83822_read(dev, DP83822_PHYSTS_REG, &physts);

		if (physts & DP83822_PHYSTS_LINK_STATUS) {
			state->is_up = true;
			state->speed = old_state.speed;
		}

		LOG_DBG("PHY (%d) link-down diag: bmsr=0x%04X reread=0x%04X physts=0x%04X -> %s",
			config->addr, bmsr, bmsr2, physts,
			(physts & DP83822_PHYSTS_LINK_STATUS) ? "UP (latched blip)"
							      : "DOWN (real drop)");
		ret = 0;
		goto unlock;
	}

	ret = phy_dp83822_read(dev, MII_BMCR, &bmcr);
	if (ret) {
		LOG_ERR("Error reading phy (%d) control register", config->addr);
		goto unlock;
	}

	if (!(bmcr & MII_BMCR_AUTONEG_ENABLE)) {
		/* Forced mode: speed/duplex come straight from BMCR */
		if (bmcr & MII_BMCR_SPEED_100) {
			state->speed = (bmcr & MII_BMCR_DUPLEX_MODE)
					       ? LINK_FULL_100BASE : LINK_HALF_100BASE;
		} else {
			state->speed = (bmcr & MII_BMCR_DUPLEX_MODE)
					       ? LINK_FULL_10BASE : LINK_HALF_10BASE;
		}
		goto unlock;
	}

	ret = phy_dp83822_read(dev, MII_ANAR, &anar);
	if (ret) {
		LOG_ERR("Error reading phy (%d) advertising register", config->addr);
		goto unlock;
	}

	ret = phy_dp83822_read(dev, MII_ANLPAR, &anlpar);
	if (ret) {
		LOG_ERR("Error reading phy (%d) link partner register", config->addr);
		goto unlock;
	}

ret = phy_dp83822_read(dev, MII_ANLPAR, &anlpar);
	if (ret) {
		LOG_ERR("Error reading phy (%d) link partner register", config->addr);
		goto unlock;
	}

	/* --- ADD THESE REGISTER READS HERE --- */
	uint32_t recr = 0;
	uint32_t rcsr = 0;
	(void)phy_dp83822_read(dev, 0x15, &recr); /* RECR: Receive Error Counter */
	(void)phy_dp83822_read(dev, 0x17, &rcsr); /* RCSR: RMII / FIFO Status   */

	if ((pr_counter++ % 4) == 0) {
		LOG_DBG("PHY (%d) ANAR 0x%04X ANLPAR 0x%04X | RECR 0x%04X RCSR 0x%04X",
			config->addr, anar, anlpar, recr, rcsr);
	}
	/* ------------------------------------- */

	// if ((pr_counter++ % 4) == 0) {
	// 	LOG_DBG("PHY (%d) ANAR 0x%04X ANLPAR 0x%04X", config->addr, anar, anlpar);
	// }

	uint32_t mutual = anar & anlpar;

	if (mutual & MII_ADVERTISE_100_FULL) {
		state->speed = LINK_FULL_100BASE;
	} else if (mutual & MII_ADVERTISE_100_HALF) {
		state->speed = LINK_HALF_100BASE;
	} else if (mutual & MII_ADVERTISE_10_FULL) {
		state->speed = LINK_FULL_10BASE;
	} else if (mutual & MII_ADVERTISE_10_HALF) {
		state->speed = LINK_HALF_10BASE;
	} else {
		ret = -EIO;
	}

unlock:
	k_mutex_unlock(&data->mutex);

	if (ret == 0 && memcmp(&old_state, state, sizeof(*state)) != 0) {
		LOG_DBG("PHY %d is %s", config->addr, state->is_up ? "up" : "down");
		if (state->is_up) {
			LOG_DBG("PHY (%d) link %s Mb %s duplex", config->addr,
				PHY_LINK_IS_SPEED_100M(state->speed) ? "100" : "10",
				PHY_LINK_IS_FULL_DUPLEX(state->speed) ? "full" : "half");
		}
	}

	return ret;
}

static int __maybe_unused phy_dp83822_sw_reset(const struct device *dev)
{
	const struct dp83822_config *config = dev->config;
	struct dp83822_data *data = dev->data;
	uint32_t bmcr = 0;
	uint16_t timeout = DP83822_SW_RESET_TIMEOUT_MS / DP83822_SW_RESET_POLL_MS;
	int ret;

	ret = k_mutex_lock(&data->mutex, K_FOREVER);
	if (ret) {
		LOG_ERR("PHY mutex lock error");
		return ret;
	}

	ret = phy_dp83822_write(dev, MII_BMCR, MII_BMCR_RESET);
	if (ret) {
		LOG_ERR("PHY (%d) software reset write failed: %d", config->addr, ret);
		goto out;
	}

	do {
		if (timeout-- == 0) {
			LOG_ERR("PHY (%d) software reset timed out", config->addr);
			ret = -ETIMEDOUT;
			goto out;
		}
		k_msleep(DP83822_SW_RESET_POLL_MS);

		ret = phy_dp83822_read(dev, MII_BMCR, &bmcr);
		if (ret) {
			LOG_ERR("PHY (%d) BMCR read during reset failed: %d",
				config->addr, ret);
			goto out;
		}
	} while (bmcr & MII_BMCR_RESET);

	k_busy_wait(DP83822_POST_RESET_DELAY_US);
	LOG_DBG("PHY (%d) software reset complete", config->addr);

out:
	k_mutex_unlock(&data->mutex);
	return ret;
}

/* -----------------------------------------------------------------------
 * PHY API
 * ----------------------------------------------------------------------- */
static int phy_dp83822_cfg_link(const struct device *dev,
				enum phy_link_speed speeds,
				enum phy_cfg_link_flag flags)
{
	const struct dp83822_config *config = dev->config;
	struct dp83822_data *data = dev->data;
	struct phy_link_state state = {};
	uint32_t anar = 0;
	bool autoneg = !(flags & PHY_FLAG_AUTO_NEGOTIATION_DISABLED);
	int ret;

	ret = k_mutex_lock(&data->mutex, K_FOREVER);
	if (ret) {
		LOG_ERR("PHY (%d) mutex lock failed: %d", config->addr, ret);
		return ret;
	}

	if (data->cfg_valid && data->cfg_speeds == speeds && data->cfg_flags == flags) {
		k_mutex_unlock(&data->mutex);
		return -EALREADY;
	}

	k_work_cancel_delayable(&data->phy_monitor_work);

	if (autoneg) {
		ret = phy_dp83822_read(dev, MII_ANAR, &anar);
		if (ret) {
			LOG_ERR("PHY (%d) ANAR read failed: %d", config->addr, ret);
			goto done;
		}

		WRITE_BIT(anar, __builtin_ctz(MII_ADVERTISE_100_FULL),
			  speeds & LINK_FULL_100BASE);
		WRITE_BIT(anar, __builtin_ctz(MII_ADVERTISE_100_HALF),
			  speeds & LINK_HALF_100BASE);
		WRITE_BIT(anar, __builtin_ctz(MII_ADVERTISE_10_FULL),
			  speeds & LINK_FULL_10BASE);
		WRITE_BIT(anar, __builtin_ctz(MII_ADVERTISE_10_HALF),
			  speeds & LINK_HALF_10BASE);

		ret = phy_dp83822_write(dev, MII_ANAR, anar);
		if (ret) {
			LOG_ERR("PHY (%d) ANAR write failed: %d", config->addr, ret);
			goto done;
		}

		ret = phy_dp83822_autonegotiate(dev);
		if (ret && (ret != -ENETDOWN)) {
			LOG_ERR("PHY (%d) auto-negotiation error: %d", config->addr, ret);
			goto done;
		}
	} else {
		ret = phy_dp83822_force_link(dev, speeds);
		if (ret) {
			LOG_ERR("PHY (%d) forced link config failed: %d", config->addr, ret);
			goto done;
		}
	}

	data->cfg_speeds = speeds;
	data->cfg_flags = flags;
	data->cfg_valid = true;

	ret = phy_dp83822_get_link(dev, &state);
	if (ret == 0 && memcmp(&state, &data->state, sizeof(state)) != 0) {
		data->state = state;
		if (data->cb) {
			data->cb(dev, &data->state, data->cb_data);
		}
	}

	LOG_INF("PHY %d is %s", config->addr, data->state.is_up ? "up" : "down");
	if (data->state.is_up) {
		LOG_INF("PHY (%d) link %s Mb %s duplex", config->addr,
			PHY_LINK_IS_SPEED_100M(data->state.speed) ? "100" : "10",
			PHY_LINK_IS_FULL_DUPLEX(data->state.speed) ? "full" : "half");
	}

done:
	k_mutex_unlock(&data->mutex);

	k_work_reschedule(&data->phy_monitor_work, K_MSEC(CONFIG_PHY_MONITOR_PERIOD));
	return ret;
}

static int phy_dp83822_link_cb_set(const struct device *dev,
				   phy_callback_t cb, void *user_data)
{
	struct dp83822_data *data = dev->data;

	data->cb = cb;
	data->cb_data = user_data;

	(void)phy_dp83822_get_link(dev, &data->state);
	if (data->cb) {
		data->cb(dev, &data->state, data->cb_data);
	}

	/* Keep polling so a later link-up is reported to the MAC */
	k_work_reschedule(&data->phy_monitor_work,
			  K_MSEC(CONFIG_PHY_MONITOR_PERIOD));

	return 0;
}

static void phy_dp83822_monitor_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct dp83822_data *data =
		CONTAINER_OF(dwork, struct dp83822_data, phy_monitor_work);
	const struct device *dev = data->dev;
	struct phy_link_state state = {};
	int rc;

	rc = phy_dp83822_get_link(dev, &state);

	if (rc == 0 && memcmp(&state, &data->state, sizeof(state)) != 0) {
		data->state = state;
		if (data->cb) {
			data->cb(dev, &data->state, data->cb_data);
		}
	}

	/* TODO: replace polling with GPIO interrupt when hardware allows */
	k_work_reschedule(&data->phy_monitor_work, K_MSEC(CONFIG_PHY_MONITOR_PERIOD));
}

/* -----------------------------------------------------------------------
 * Init
 * ----------------------------------------------------------------------- */
static int phy_dp83822_init(const struct device *dev)
{
	const struct dp83822_config *config = dev->config;
	struct dp83822_data *data = dev->data;
	uint32_t sor1 = 0;
	int ret;

	data->dev = dev;
	data->cfg_valid = false;

	if (!device_is_ready(config->mdio_dev)) {
		LOG_ERR("MDIO bus device not ready");
		return -ENODEV;
	}

	k_mutex_init(&data->mutex);
	k_work_init_delayable(&data->phy_monitor_work, phy_dp83822_monitor_work_handler);

	k_work_reschedule(&data->phy_monitor_work, K_MSEC(CONFIG_PHY_MONITOR_PERIOD));
	// mdio_bus_enable(config->mdio_dev);

	/* Software reset intentionally skipped (it would reload strap defaults):
	 * ret = phy_dp83822_sw_reset(dev);
	 */

	/* SOR1 lives in extended register space (0x0467); it must be accessed
	 * through REGCR/ADDAR, not with a direct Clause 22 MDIO frame.
	 */
	ret = phy_dp83822_ext_read(dev, DP83822_SOR1_REG, &sor1);
	if (ret) {
		LOG_ERR("PHY (%d) SOR1 read failed: %d", config->addr, ret);
		return ret;
	}
	LOG_DBG("PHY (%d) SOR1 before: 0x%04X", config->addr, sor1);

	sor1 &= 0xFF3FU;
	sor1 |= 0x0080U;

	ret = phy_dp83822_ext_write(dev, DP83822_SOR1_REG, sor1);
	if (ret) {
		LOG_ERR("PHY (%d) SOR1 write failed: %d", config->addr, ret);
		return ret;
	}
	LOG_DBG("PHY (%d) SOR1 after: 0x%04X", config->addr, sor1);

	return 0;
}

static DEVICE_API(ethphy, dp83822_phy_api) = {
	.get_link    = phy_dp83822_get_link,
	.cfg_link    = phy_dp83822_cfg_link,
	.link_cb_set = phy_dp83822_link_cb_set,
	.read        = phy_dp83822_read,
	.write       = phy_dp83822_write,
};

#define TI_DP83822_INIT(n)                                                     \
	static const struct dp83822_config dp83822_##n##_config = {            \
		.addr      = DT_INST_REG_ADDR(n),                              \
		.mdio_dev  = DEVICE_DT_GET(DT_INST_PARENT(n)),                 \
		.rmii_mode = DT_INST_ENUM_IDX_OR(n, ti_rmii_mode, 0),          \
	};                                                                     \
                                                                               \
	static struct dp83822_data dp83822_##n##_data;                         \
                                                                               \
	DEVICE_DT_INST_DEFINE(n, &phy_dp83822_init, NULL,                      \
			      &dp83822_##n##_data, &dp83822_##n##_config,      \
			      POST_KERNEL, CONFIG_PHY_INIT_PRIORITY,           \
			      &dp83822_phy_api);

DT_INST_FOREACH_STATUS_OKAY(TI_DP83822_INIT)