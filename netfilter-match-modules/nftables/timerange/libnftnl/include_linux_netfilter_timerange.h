#ifndef _TIMERANGE_H
#define _TIMERANGE_H

enum nft_timerange_attributes {
	NFTA_TIMERANGE_UNSPEC,
	NFTA_TIMERANGE_FLAGS,
	NFTA_TIMERANGE_HOURS,
	NFTA_TIMERANGE_WEEKDAYS,
	NFTA_TIMERANGE_WEEKLYRANGES,
	__NFTA_TIMERANGE_MAX,
};

#define NFTA_TIMERANGE_MAX (__NFTA_TIMERANGE_MAX - 1)

#endif /* _TIMERANGE_H */