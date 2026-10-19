/*
 * CAF-only power supply values used by the WCD microphone bias code to spot a
 * USB-C audio adapter. Upstream's power_supply.h has none of these, and adding
 * a POWER_SUPPLY_PROP_* bit would change the property bitmap ABI, so they are
 * kept private to the ported driver with the values CAF uses.
 *
 * From the donor's include/linux/power_supply.h.
 */
#ifndef _WCD_MBHC_POWER_SUPPLY_COMPAT_H
#define _WCD_MBHC_POWER_SUPPLY_COMPAT_H

/* Indicates USB Type-C CC connection status */
enum power_supply_typec_mode {
	POWER_SUPPLY_TYPEC_NONE,

	/* Acting as source */
	POWER_SUPPLY_TYPEC_SINK,
	POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE,
	POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY,
	POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER,
	POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY,

	/* Acting as sink */
	POWER_SUPPLY_TYPEC_SOURCE_DEFAULT,
	POWER_SUPPLY_TYPEC_SOURCE_MEDIUM,
	POWER_SUPPLY_TYPEC_SOURCE_HIGH,
	POWER_SUPPLY_TYPEC_NON_COMPLIANT,
};

enum power_supply_typec_power_role {
	POWER_SUPPLY_TYPEC_PR_NONE,	/* CC lines in high-Z */
	POWER_SUPPLY_TYPEC_PR_DUAL,
	POWER_SUPPLY_TYPEC_PR_SINK,
	POWER_SUPPLY_TYPEC_PR_SOURCE,
};

/*
 * CAF's property bit. This tree's power_supply core does not implement it, so
 * the ported code only ever uses it as a value the WCD driver writes for its
 * own bookkeeping.
 */
#define POWER_SUPPLY_PROP_TYPEC_POWER_ROLE 0x00000080

#endif /* _WCD_MBHC_POWER_SUPPLY_COMPAT_H */
