#!/bin/sh

. /usr/share/libubox/jshn.sh

print_mac80211_capabs_for_wifi_dev()
{
	wifi_dev="$1"
	dev_num="$2"
	out="$3"

	phyname=$(iwinfo nl80211 phyname $wifi_dev 2>&1)
	[ "$phyname" = "Phy not found" ] && phyname="phy$dev_num"
	# Check for wl naming
	[ "$(iwinfo $phyname h 2>&1)" = "No such wireless device: $phyname" ] && phyname="wl$dev_num"
	echo "phyRadio[\"$phyname\"] = \"$wifi_dev\";" >> "$out"
	echo "radioPhy[\"$wifi_dev\"] = \"$phyname\";" >> "$out"
	echo "phyCapab[\"$phyname\"] = [];" >> "$out"

	json_load "$(cat /etc/board.json)"
	json_select wlan || echo "/etc/board.json does not contain WLAN, cannot parse"
	json_select "$phyname" && {
		json_select info
		json_select bands
		json_get_keys radiobands
		for b in $radiobands; do
			json_select $b
			echo "phyCapab[\"$phyname\"][\"$b\"] = [];" >> "$out"
			json_get_var ht ht
			json_get_var vht vht
			json_get_var he he
			json_get_var maxwidth max_width
			echo "phyCapab[\"$phyname\"][\"$b\"][\"ht\"] = ${ht:-0};" >> "$out"
			echo "phyCapab[\"$phyname\"][\"$b\"][\"vht\"] = ${vht:-0};" >> "$out"
			echo "phyCapab[\"$phyname\"][\"$b\"][\"he\"] = ${he:-0};" >> "$out"
			echo "phyCapab[\"$phyname\"][\"$b\"][\"max_width\"] = $maxwidth;" >> "$out"
			json_select modes
			json_get_keys modeidx
			echo "phyCapab[\"$phyname\"][\"$b\"][\"modes\"] = [];" >> "$out"
			for m in $modeidx; do
				json_get_var modeval $m
				echo "phyCapab[\"$phyname\"][\"$b\"][\"modes\"].push(\"$modeval\");" >> "$out"
			done
			json_select ..
			json_select ..

			echo "phyCapab[\"$phyname\"][\"$b\"][\"channels\"] = [];" >> "$out"
			echo "phyCapab[\"$phyname\"][\"$b\"][\"freqs\"] = [];" >> "$out"
			echo "phyCapab[\"$phyname\"][\"$b\"][\"pwrs\"] = [];" >> "$out"

			echo "nextCh     = [];" >> "$out"
			echo "nextChFreq = [];" >> "$out"
			echo "nextChPwr  = [];" >> "$out"

			# we are about to screen-scrape iw output, which the tool specifically says we should NOT do
			# however, as far as I can tell there is no other way to get max txpower for each channel
			# so... here it goes.
			# If stuff gets FUBAR, take a look at iw output, and see if this god-awful expression still works
			iw "$phyname" info 2>&1 | sed -e '/MHz/!d; /GI/d; /disabled/d; /radar detect /d; /Supported Channel Width/d; /STBC/d; /PPDU/d; /MCS/d; s/[:blank:]*\*[:blank:]*//g; s:[]()[]::g; s/\.0//g; s/ dBm.*//g;' | grep 'MHz' | awk ' { print "nextCh.push("$3"); nextChFreq["$3"] = \""$1"MHz\"; nextChPwr["$3"] = "$4";"   ; } ' >> "$out"

			echo "phyCapab[\"$phyname\"][\"$b\"][\"channels\"] = nextCh ;"     >> "$out"
			echo "phyCapab[\"$phyname\"][\"$b\"][\"freqs\"]  = nextChFreq ;" >> "$out"
			echo "phyCapab[\"$phyname\"][\"$b\"][\"pws\"]   = nextChPwr ;"  >> "$out"
		done
	}
}

print_mac80211_channels_for_wifi_dev()
{
	wifi_dev="$1"
	dev_num="$2"
	out="$3"
	dualband="$4"
	
	echo "nextCh     = [];" >> "$out"
	echo "nextChFreq = [];" >> "$out"
	echo "nextChPwr  = [];" >> "$out"
	mode=$(uci get wireless.$wifi_dev.band)
	phyname=$(iwinfo nl80211 phyname $wifi_dev 2>&1)
	[ "$phyname" = "Phy not found" ] && phyname="phy$dev_num"
	# Check for wl naming
	[ "$(iwinfo $phyname h 2>&1)" = "No such wireless device: $phyname" ] && phyname="wl$dev_num"

	htpat="\bHT[0-9]\{2,3\}"
	vhtpat="\bVHT[0-9]\{2,3\}\(+[0-9]\{2\}\)\?"
	hepat="\bHE[0-9]\{2,3\}\(+[0-9]\{2\}\)\?"
	wifiN=$(iwinfo $wifi_dev h | sed -e "s/$vhtpat//g" -e "s/$hepat//g" -e 's/^[ ]*//')
	wifiAC=$(iwinfo $wifi_dev h | sed -e "s/$htpat//g" -e "s/$hepat//g" -e 's/^[ ]*//')
	wifiAX=$(iwinfo $wifi_dev h | sed -e "s/$htpat//g" -e "s/$vhtpat//g" -e 's/^[ ]*//')
	if [ "$wifiAC" ] ; then
		maxAC=$(echo $wifiAC | awk -F " VHT" '{print $NF}')
		AC80P80=$(echo $wifiAC | grep "VHT80+80")
	else
		maxAC="0"
		AC80P80=""
	fi
	if [ "$wifiAX" ] ; then
		maxAX=$(echo $wifiAX | awk -F " HE" '{print $NF}')
		AX80P80=$(echo $wifiAX | grep "HE80+80")
	else
		maxAX="0"
		AX80P80=""
	fi

	#802.11ac should only be able to operate on the "A" device
	
	if [ "$mode" = "5g" ] ; then
		chId="A"
		echo "wifiDevA=\"$wifi_dev\";" >> "$out"
		if [ "$wifiN" ] ; then
			echo "var AwifiN = true;" >> "$out"
		else
			echo "var AwifiN = false;" >> "$out"
		fi
		if [ "$wifiAC" ] ; then
			echo "var AwifiAC = true;" >> "$out"
		else
			echo "var AwifiAC = false;" >> "$out"
		fi
		if [ "$wifiAX" ] ; then
			echo "var AwifiAX = true;" >> "$out"
		else
			echo "var AwifiAX = false;" >> "$out"
		fi
		if [ "$dualband" == false ] ; then
			echo "var GwifiN = false;" >> "$out"
			echo "var GwifiAX = false;" >> "$out"
		fi
		echo "var maxACwidth = \"$maxAC\" ;" >> "$out"
		if [ "$AC80P80" ] ; then
			echo "var AC80P80 = true;" >> "$out"
		else
			echo "var AC80P80 = false;" >> "$out"
		fi
		echo "var maxAXwidth = \"$maxAX\" ;" >> "$out"
		if [ "$AX80P80" ] ; then
			echo "var AX80P80 = true;" >> "$out"
		else
			echo "var AX80P80 = false;" >> "$out"
		fi
	else
		chId="G"
		echo "wifiDevG=\"$wifi_dev\";" >> "$out"
		if [ "$wifiN" ] ; then
			echo "var GwifiN = true;" >> "$out"
		else
			echo "var GwifiN = false;" >> "$out"
		fi
		if [ "$wifiAX" ] ; then
			echo "var GwifiAX = true;" >> "$out"
		else
			echo "var GwifiAX = false;" >> "$out"
		fi
		if [ "$dualband" == false ] ; then
			echo "var AwifiN = false;" >> "$out"
			echo "var AwifiAC = false;" >> "$out"
			echo "var AwifiAX = false;" >> "$out"
		fi
	fi
	
	# we are about to screen-scrape iw output, which the tool specifically says we should NOT do
	# however, as far as I can tell there is no other way to get max txpower for each channel
	# so... here it goes.
	# If stuff gets FUBAR, take a look at iw output, and see if this god-awful expression still works
	iw "$phyname" info 2>&1 | sed -e '/MHz/!d; /GI/d; /disabled/d; /radar detect /d; /Supported Channel Width/d; /STBC/d; /PPDU/d; /MCS/d; s/[:blank:]*\*[:blank:]*//g; s:[]()[]::g; s/\.0//g; s/ dBm.*//g;' | grep 'MHz' | awk ' { print "nextCh.push("$3"); nextChFreq["$3"] = \""$1"MHz\"; nextChPwr["$3"] = "$4";"   ; } ' >> "$out"

	echo "mac80211Channels[\"$chId\"] = nextCh ;"     >> "$out"
	echo "mac80211ChFreqs[\"$chId\"]  = nextChFreq ;" >> "$out"
	echo "mac80211ChPwrs[\"$chId\"]   = nextChPwr ;"  >> "$out"
}

out_file="/var/cached_basic_vars"
if [ -e "$out_file" ] ; then
	noDriver="$(grep "wirelessDriver=..;" "$out_file" 2>/dev/null)"
	echo "no driver = $noDriver"
	if [ -z "$noDriver" ] ; then exit ; else rm -f "$out_file" ; fi
fi
touch "$out_file"

# determine if this board is a bcm94704, for which the uci wan macaddr variable must ALWAYS be set
PART="$(grep 'nvram' /proc/mtd)"
PART="${PART%%:*}"
if [ -n "$PART" ] ; then
	PREFIX=/dev/mtdblock
	PART="${PART##mtd}"
	[ -d /dev/mtdblock ] && PREFIX=/dev/mtdblock/ 
	nvrampath="${PART:+$PREFIX$PART}"
	boardtype="$(strings "${nvrampath}" | sed -e '/boardtype/!d; s#boardtype=##g')"
	boardnum="$(strings "${nvrampath}" | sed -e '/boardnum/!d; s#boardnum=##g')"
	#echo "boardnum = $boardnum, boardtype = $boardtype"
	isbcm94704='false'
	if [ "$boardtype" = "0x0472" ] || [ "$boardtype" = "0x042f" ] ; then
		if [ "$boardnum" != "45" ] ; then
			isbcm94704='true'
		fi
	fi
else
	isbcm94704='false'
fi
echo "var isBcm94704 = $isbcm94704;" >> "$out_file"
echo "var allLanMacs = [];" >> "$out_file"
brctl showmacs br-lan | grep "yes" | awk ' { print "allLanMacs.push(\"" $2 "\");" } ' >> "$out_file"

# determine if this board is a ramips, for which the uci wan macaddr variable must ALWAYS be set
ramips='false'
[ -f /lib/ramips.sh ] && ramips='true'
echo "var isRamips = $ramips;" >> "$out_file"

echo "var wifiDevG=uciWirelessDevs.length > 0 ? uciWirelessDevs[0] : \"\";" >> "$out_file"
echo "var wifiDevA=\"\";" >> "$out_file"
echo "var phyRadio=[];" >> "$out_file"
echo "var radioPhy=[];" >> "$out_file"
echo "var phyCapab=[];" >> "$out_file"

if [ -e /lib/wifi/broadcom.sh ] ; then
	echo "var wirelessDriver=\"broadcom\";" >> "$out_file"
	echo "var GwifiN = false;" >> "$out_file"
	echo "var GwifiAX = false;" >> "$out_file"
	echo "var AwifiN = false;" >> "$out_file"
	echo "var AwifiAC = false;" >> "$out_file"
	echo "var AwifiAX = false;" >> "$out_file"
	echo "var dualBandWireless=false;" >> "$out_file"
elif [ -e /lib/wifi/mac80211.uc ] && [ -e "/sys/class/ieee80211/phy0" -o -e "/sys/class/ieee80211/wl0" ] ; then
	echo 'var wirelessDriver="mac80211";' >> "$out_file"
	echo 'var mac80211Channels = [];' >> "$out_file"
	echo 'var mac80211ChFreqs = [];' >> "$out_file"
	echo 'var mac80211ChPwrs = [];' >> "$out_file"

	echo "var nextCh=[];" >> "$out_file"
	
	#test for dual band
	if [ "$(uci show wireless | grep wifi-device | wc -l)" = "2" ] && [ -e "/sys/class/ieee80211/phy1" -o -e "/sys/class/ieee80211/wl1" ] && [ ! "$(uci get wireless.@wifi-device[0].band)" = "$(uci get wireless.@wifi-device[1].band)"  ] ; then
		echo "var dualBandWireless=true;" >> "$out_file"
		dualband='true'
	else
		echo "var dualBandWireless=false;" >> "$out_file"
		dualband='false'
	fi
	
	radios="$(uci show wireless | sed -e '/wifi-device/!d; s/^.*\.//g; s/=.*$//g')"
	rnum=0;
	for r in $radios ; do
		print_mac80211_channels_for_wifi_dev "$r" "$rnum" "$out_file" "$dualband"
		print_mac80211_capabs_for_wifi_dev "$r" "$rnum" "$out_file"
		rnum=$(( $rnum+1 ))
	done
else
	echo "var wirelessDriver=\"\";" >> "$out_file"
	echo "var GwifiN = false;" >> "$out_file"
	echo "var GwifiAX = false;" >> "$out_file"
	echo "var AwifiN = false;" >> "$out_file"
	echo "var AwifiAC = false;" >> "$out_file"
	echo "var AwifiAX = false;" >> "$out_file"
	echo "var dualBandWireless=false;" >> "$out_file"
fi

echo "var wpad_eap = $(hostapd -veap && echo 'true' || echo 'false');" >> "$out_file"
echo "var wpad_sae = $(hostapd -vsae && echo 'true' || echo 'false');" >> "$out_file"
echo "var wpad_owe = $(hostapd -vowe && echo 'true' || echo 'false');" >> "$out_file"
echo "var wpad_sb192 = $(hostapd -vsuiteb192 && echo 'true' || echo 'false');" >> "$out_file"
echo "var wpad_wep = $(hostapd -vwep && echo 'true' || echo 'false');" >> "$out_file"

awk -F= '/DISTRIB_TARGET/{printf "var distribTarget=%s;\n", $2}' /etc/openwrt_release >> "$out_file"

# cache default interfaces if we haven't already
# this script is run on first boot by hotplug, so
# this will make sure the defaults get cached right
# away
gargoyle_header_footer -i >/dev/null 2>&1
