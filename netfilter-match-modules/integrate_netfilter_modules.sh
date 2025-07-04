insert_lines_at()
{
	insert_after=$1
	lines=$2
	file=$3
	default_end=$4 #if line not specified, 0=put at end 1=put at beginning, default=end

	file_length=$(cat $file | wc -l | sed 's/ .*$//g')
	if [ -z "$insert_after" ] ; then
		if [ $default_end = "0" ] ; then
			$insert_after=0
		else
			$insert_after=$(($file_length+1))
		fi
	fi
	remainder=$(($file_length - $insert_after))
		
	head -n $insert_after $file >.tmp.tmp
	printf "$lines\n" >>.tmp.tmp
	if [ $remainder -gt 0 ] ; then
		tail -n $remainder $file >>.tmp.tmp
	fi
	mv .tmp.tmp $file
}


#define paths
openwrt_buildroot_dir="$1"
module_dir="$2"

#patch modes
patch_openwrt="$3"
patch_kernel="$4"
if [ -z "$patch_openwrt" ] ; then
	patch_openwrt="1"
fi
if [ -z "$patch_kernel" ] ; then
	patch_kernel="1"
fi



if [ -z "$openwrt_buildroot_dir" ] ; then
	echo "ERROR: you must specify OpenWrt buildroot directory"
	exit
fi
if [ -z "$module_dir" ] ; then
	echo "ERROR: you must specify module source directory"
	exit
fi

if [ ! -e "$openwrt_buildroot_dir/.config" ] ; then
	echo "ERROR: you must have a build configuration specified to run this script (run make menuconfig or make sure you have a .config file in the buildroot dir"
	exit
fi

iptables_module_dir="$module_dir/iptables"
nftables_module_dir="$module_dir/nftables"

new_iptables_module_dirs=""
new_nftables_module_dirs=""
# IPTABLES
new_module_list=$(ls "$iptables_module_dir" 2>/dev/null)
for d in $new_module_list ; do
	if [ -d "$iptables_module_dir/$d" ] ; then
		new_name=$(cat $iptables_module_dir/$d/name 2>/dev/null)
		if [ -n "$new_name" ] ; then
			new_iptables_module_dirs="$d $new_iptables_module_dirs"
		fi	
	fi
done
# NFTABLES
new_module_list=$(ls "$nftables_module_dir" 2>/dev/null)
for d in $new_module_list ; do
	if [ -d "$nftables_module_dir/$d" ] ; then
		new_name=$(cat $nftables_module_dir/$d/name 2>/dev/null)
		if [ -n "$new_name" ] ; then
			new_nftables_module_dirs="$d $new_nftables_module_dirs"
		fi	
	fi
done
if [ -z "$new_iptables_module_dirs" ] && [ -z "$new_nftables_module_dirs" ] ; then
	#nothing to do, exit cleanly without error
	exit
fi

#make paths absolute
exec_dir=$(pwd);
cd "$openwrt_buildroot_dir"
openwrt_buildroot_dir=$(pwd)
cd "$exec_dir"
cd "$module_dir"
module_dir=$(pwd)
cd "$exec_dir"


cd "$openwrt_buildroot_dir"
mkdir -p nf-patch-build
rm -rf nf-patch-build/* 2>/dev/null #should be nothing there, should fail with error (which just gets dumped to /dev/null), but let's be sure

if [ ! -d dl ] ; then
	mkdir dl
fi

####################################################################################################
##### CREATE MAKEFILE THAT WILL DOWNLOAD LINUX SOURCE FOR TARGET SPECIFIED IN .config FILE #########
####################################################################################################

if [ "$patch_kernel" = 1 ] ; then
	target_name=$(cat .config | egrep  "CONFIG_TARGET_([^_]+)=y" | sed 's/^.*_//g' | sed 's/=y$//g' )
	if [ -z "$target_name" ] ; then
		test_names=$(cat .config | egrep  "CONFIG_TARGET_.*=y" | sed 's/CONFIG_TARGET_//g' | sed 's/_.*$//g' )
		for name in $test_names ; do
			for kernel in 2.2 2.4 2.6 2.8 3.0 3.2 3.4 ; do  #let's plan ahead!!!
				if [ -d "target/linux/$name-$kernel" ] ; then
					target_name="$name-$kernel"
				fi
			done
		done
	fi


	board_var=$(cat target/linux/$target_name/Makefile | grep "BOARD.*:=")
	kernel_var=$(cat target/linux/$target_name/Makefile | grep "KERNEL.*:=")
	linux_ver_var=$(cat target/linux/$target_name/Makefile | grep "LINUX_VERSION.*:=") 
	defines=$(printf "$board_var\n$kernel_var\n$linux_ver_var\n")

	cat << 'EOF' >nf-patch-build/linux-download-make
CP:=cp -fpR
TOPDIR:=..
INCLUDE_DIR:=$(TOPDIR)/include
SCRIPT_DIR:=$(TOPDIR)/scripts
DL_DIR:=$(TOPDIR)/dl
STAGING_DIR_HOST:=$(TOPDIR)/staging_dir/host
MKHASH:=$(STAGING_DIR_HOST)/bin/mkhash
# MKHASH is used in /scripts, so we export it here.
export MKHASH
# DOWNLOAD_CHECK_CERTIFICATE is used in /scripts, so we export it here.
DOWNLOAD_CHECK_CERTIFICATE:=$(CONFIG_DOWNLOAD_CHECK_CERTIFICATE)
export DOWNLOAD_CHECK_CERTIFICATE
EOF

	printf "$defines\n" >> nf-patch-build/linux-download-make

	cat << 'EOF' >>nf-patch-build/linux-download-make
include $(INCLUDE_DIR)/kernel-version.mk
KERNEL_NAME:=$(shell echo "$(KERNEL)" | sed 's/ /\./g' |  sed 's/\.$$//g' )
KERNEL_PATCHVER_NAME:=$(shell echo "$(KERNEL_PATCHVER)" | sed 's/ /\./g' |  sed 's/\.$$//g' )

GENERIC_PLATFORM_DIR := $(TOPDIR)/target/linux/generic
PLATFORM_DIR:=$(TOPDIR)/target/linux/$(BOARD)

GENERIC_BACKPORT_PATCH_DIR := $(GENERIC_PLATFORM_DIR)/backport-$(KERNEL_NAME)
GENERIC_PENDING_PATCH_DIR := $(GENERIC_PLATFORM_DIR)/pending-$(KERNEL_NAME)
GENERIC_HACK_PATCH_DIR := $(GENERIC_PLATFORM_DIR)/hack-$(KERNEL_NAME)
GENERIC_PATCH_DIR := $(GENERIC_PLATFORM_DIR)/patches-$(KERNEL_NAME)

GENERIC_FILES_DIR := $(GENERIC_PLATFORM_DIR)/files
GENERIC_LINUX_CONFIG:=$(firstword $(wildcard $(GENERIC_PLATFORM_DIR)/config-$(KERNEL_PATCHVER_NAME) $(GENERIC_PLATFORM_DIR)/config-default))
PATCH_DIR := $(PLATFORM_DIR)/patches$(shell [ -d "$(PLATFORM_DIR)/patches-$(KERNEL_PATCHVER_NAME)" ] && printf -- "-$(KERNEL_PATCHVER_NAME)" || true )
FILES_DIR := $(PLATFORM_DIR)/files$(shell [ -d "$(PLATFORM_DIR)/files-$(KERNEL_PATCHVER_NAME)" ] && printf -- "-$(KERNEL_PATCHVER_NAME)" || true )
LINUX_CONFIG:=$(firstword $(wildcard $(foreach subdir,$(PLATFORM_DIR) $(PLATFORM_SUBDIR),$(subdir)/config-$(KERNEL_PATCHVER_NAME) $(subdir)/config-default)) $(PLATFORM_DIR)/config-$(KERNEL_PATCHVER_NAME))
LINUX_DIR:=linux
PKG_BUILD_DIR:=$(LINUX_DIR)
TARGET_BUILD:=1
LINUX_SOURCE:=linux-$(LINUX_VERSION).tar.xz
TESTING:=$(if $(findstring -rc,$(LINUX_VERSION)),/testing,)
ifeq ($(call qstrip,$(CONFIG_EXTERNAL_KERNEL_TREE))$(call qstrip,$(CONFIG_KERNEL_GIT_CLONE_URI)),)
  ifeq ($(word 1,$(subst ., ,$(KERNEL_BASE))),3)
    LINUX_SITE:=@KERNEL/linux/kernel/v3.x$(TESTING)
  else
    LINUX_SITE:=@KERNEL/linux/kernel/v$(word 1,$(subst ., ,$(KERNEL_BASE))).x$(TESTING)
  endif
endif


define filter_series
sed -e s,\\#.*,, $(1) | grep -E \[a-zA-Z0-9\]
endef

all:
	if [ ! -e "$(DL_DIR)/$(LINUX_SOURCE)" ] ; then TOPDIR="$(TOPDIR)"  $(SCRIPT_DIR)/download.pl $(DL_DIR) $(LINUX_SOURCE) $(LINUX_KERNEL_HASH) $(LINUX_SOURCE) $(LINUX_SITE) ; fi ; 
	cp $(DL_DIR)/$(LINUX_SOURCE) . 
	rm -rf linux linux-$(LINUX_VERSION)
	tar xfJ $(LINUX_SOURCE)
	if [  ! -e "$(DL_DIR)/$(LINUX_SOURCE)" ] ; then mv $(LINUX_SOURCE) "$(DL_DIR)/" ; else rm $(LINUX_SOURCE) ; fi
	mv linux-$(LINUX_VERSION) linux
	rm -rf $(PKG_BUILD_DIR)/patches; mkdir -p $(PKG_BUILD_DIR)/patches 
	if [ -d $(GENERIC_FILES_DIR) ]; then $(CP) $(GENERIC_FILES_DIR)/* $(LINUX_DIR)/; fi 
	if [ -d $(FILES_DIR) ]; then \
		$(CP) $(FILES_DIR)/* $(LINUX_DIR)/; \
		find $(LINUX_DIR)/ -name \*.rej | xargs rm -f; \
	fi

	if [ -d "$(GENERIC_BACKPORT_PATCH_DIR)" ] ; then $(SCRIPT_DIR)/patch-kernel.sh linux $(GENERIC_BACKPORT_PATCH_DIR) ; fi
	if [ -d "$(GENERIC_PENDING_PATCH_DIR)" ] ; then $(SCRIPT_DIR)/patch-kernel.sh linux $(GENERIC_PENDING_PATCH_DIR) ; fi
	if [ -d "$(GENERIC_HACK_PATCH_DIR)" ] ; then $(SCRIPT_DIR)/patch-kernel.sh linux $(GENERIC_HACK_PATCH_DIR) ; fi
	if [ -d "$(GENERIC_PATCH_DIR)" ] ; then $(SCRIPT_DIR)/patch-kernel.sh linux $(GENERIC_PATCH_DIR) ; fi
	if [ -d "$(PATCH_DIR)" ] ; then $(SCRIPT_DIR)/patch-kernel.sh linux $(PATCH_DIR) ; fi
	mkdir -p "$(PATCH_DIR)"

	echo $(GENERIC_BACKPORT_PATCH_DIR) > generic-backport-patch-dir
	echo $(GENERIC_PENDING_PATCH_DIR) > generic-pending-patch-dir
	echo $(GENERIC_HACK_PATCH_DIR) > generic-hack-patch-dir
	echo $(GENERIC_PATCH_DIR) > generic-patch-dir
	echo $(GENERIC_LINUX_CONFIG) > generic-config-file
	echo $(PATCH_DIR) > patch-dir
	echo $(LINUX_CONFIG) > config-file
EOF

	####################################################################################################
	##### NOW CREATE MAKEFILE THAT WILL DOWNLOAD IPTABLES SOURCE #######################################
	####################################################################################################
	echo 'TOPDIR:=..' >> nf-patch-build/iptables-download-make
	echo 'SCRIPT_DIR:=$(TOPDIR)/scripts' >> nf-patch-build/iptables-download-make
	echo 'DL_DIR:=$(TOPDIR)/dl' >> nf-patch-build/iptables-download-make
	echo 'STAGING_DIR_HOST:=$(TOPDIR)/staging_dir/host' >> nf-patch-build/iptables-download-make
	echo 'MKHASH:=$(STAGING_DIR_HOST)/bin/mkhash' >> nf-patch-build/iptables-download-make
	echo '# MKHASH is used in /scripts, so we export it here.' >> nf-patch-build/iptables-download-make
	echo 'export MKHASH' >> nf-patch-build/iptables-download-make
	echo '# DOWNLOAD_CHECK_CERTIFICATE is used in /scripts, so we export it here.' >> nf-patch-build/iptables-download-make
	echo 'DOWNLOAD_CHECK_CERTIFICATE:=$(CONFIG_DOWNLOAD_CHECK_CERTIFICATE)' >> nf-patch-build/iptables-download-make
	echo 'export DOWNLOAD_CHECK_CERTIFICATE' >> nf-patch-build/iptables-download-make
	egrep "CONFIG_LINUX_.*=y" .config | sed 's/=/:=/g' >> nf-patch-build/iptables-download-make

	package_include_line_num=$(cat package/network/utils/iptables/Makefile | egrep -n "include.*package.mk" | sed 's/:.*$//g' )
	head -n $package_include_line_num package/network/utils/iptables/Makefile | awk ' { if( ( $0 !~ /^include/ ) && ($0 !~ /^#/ )){ print $0 ; }} ' >> nf-patch-build/iptables-download-make

	echo '' >> nf-patch-build/iptables-download-make
	echo 'include $(TOPDIR)/include/download.mk' >> nf-patch-build/iptables-download-make
	echo '' >> nf-patch-build/iptables-download-make
	

	echo 'all:' >> nf-patch-build/iptables-download-make
	echo '	if [ ! -e "$(DL_DIR)/$(PKG_SOURCE)" ] ; then  TOPDIR="$(TOPDIR)" $(SCRIPT_DIR)/download.pl $(DL_DIR) $(PKG_SOURCE) $(PKG_HASH)  $(PKG_SOURCE)  $(PKG_SOURCE_URL) ; fi ; ' >> nf-patch-build/iptables-download-make
	echo '	cp $(DL_DIR)/$(PKG_SOURCE) . ' >>nf-patch-build/iptables-download-make
	echo '	tar xf $(PKG_SOURCE)' >>nf-patch-build/iptables-download-make
	echo '	rm -rf *.bz2 *.xz' >>nf-patch-build/iptables-download-make
	echo '	mv iptables* iptables' >>nf-patch-build/iptables-download-make
	echo '	$(SCRIPT_DIR)/patch-kernel.sh iptables $(TOPDIR)/package/network/utils/iptables/patches/' >>nf-patch-build/iptables-download-make
	echo '	echo $(TOPDIR)/package/network/utils/iptables/patches/ > iptables-patch-dir' >>nf-patch-build/iptables-download-make
	
	####################################################################################################
	##### NOW CREATE MAKEFILE THAT WILL DOWNLOAD NFTABLES SOURCE #######################################
	####################################################################################################
	echo 'TOPDIR:=..' >> nf-patch-build/nftables-download-make
	echo 'SCRIPT_DIR:=$(TOPDIR)/scripts' >> nf-patch-build/nftables-download-make
	echo 'DL_DIR:=$(TOPDIR)/dl' >> nf-patch-build/nftables-download-make
	echo 'STAGING_DIR_HOST:=$(TOPDIR)/staging_dir/host' >> nf-patch-build/nftables-download-make
	echo 'MKHASH:=$(STAGING_DIR_HOST)/bin/mkhash' >> nf-patch-build/nftables-download-make
	echo '# MKHASH is used in /scripts, so we export it here.' >> nf-patch-build/nftables-download-make
	echo 'export MKHASH' >> nf-patch-build/nftables-download-make
	echo '# DOWNLOAD_CHECK_CERTIFICATE is used in /scripts, so we export it here.' >> nf-patch-build/nftables-download-make
	echo 'DOWNLOAD_CHECK_CERTIFICATE:=$(CONFIG_DOWNLOAD_CHECK_CERTIFICATE)' >> nf-patch-build/nftables-download-make
	echo 'export DOWNLOAD_CHECK_CERTIFICATE' >> nf-patch-build/nftables-download-make
	egrep "CONFIG_LINUX_.*=y" .config | sed 's/=/:=/g' >> nf-patch-build/nftables-download-make

	package_include_line_num=$(cat package/network/utils/nftables/Makefile | egrep -n "include.*package.mk" | sed 's/:.*$//g' )
	head -n $package_include_line_num package/network/utils/nftables/Makefile | awk ' { if( ( $0 !~ /^include/ ) && ($0 !~ /^#/ )){ print $0 ; }} ' >> nf-patch-build/nftables-download-make

	echo '' >> nf-patch-build/nftables-download-make
	echo 'include $(TOPDIR)/include/download.mk' >> nf-patch-build/nftables-download-make
	echo '' >> nf-patch-build/nftables-download-make
	

	echo 'all:' >> nf-patch-build/nftables-download-make
	echo '	if [ ! -e "$(DL_DIR)/$(PKG_SOURCE)" ] ; then  TOPDIR="$(TOPDIR)" $(SCRIPT_DIR)/download.pl $(DL_DIR) $(PKG_SOURCE) $(PKG_HASH)  $(PKG_SOURCE)  $(PKG_SOURCE_URL) ; fi ; ' >> nf-patch-build/nftables-download-make
	echo '	cp $(DL_DIR)/$(PKG_SOURCE) . ' >>nf-patch-build/nftables-download-make
	echo '	tar xf $(PKG_SOURCE)' >>nf-patch-build/nftables-download-make
	echo '	rm -rf *.bz2 *.xz' >>nf-patch-build/nftables-download-make
	echo '	mv nftables* nftables' >>nf-patch-build/nftables-download-make
	echo '	mkdir -p $(TOPDIR)/package/network/utils/nftables/patches/' >>nf-patch-build/nftables-download-make
	echo '	$(SCRIPT_DIR)/patch-kernel.sh nftables $(TOPDIR)/package/network/utils/nftables/patches/' >>nf-patch-build/nftables-download-make
	echo '	echo $(TOPDIR)/package/network/utils/nftables/patches/ > nftables-patch-dir' >>nf-patch-build/nftables-download-make
	
	####################################################################################################
	##### NOW CREATE MAKEFILE THAT WILL DOWNLOAD LIBNFTNL SOURCE #######################################
	####################################################################################################
	echo 'TOPDIR:=..' >> nf-patch-build/libnftnl-download-make
	echo 'SCRIPT_DIR:=$(TOPDIR)/scripts' >> nf-patch-build/libnftnl-download-make
	echo 'DL_DIR:=$(TOPDIR)/dl' >> nf-patch-build/libnftnl-download-make
	echo 'STAGING_DIR_HOST:=$(TOPDIR)/staging_dir/host' >> nf-patch-build/libnftnl-download-make
	echo 'MKHASH:=$(STAGING_DIR_HOST)/bin/mkhash' >> nf-patch-build/libnftnl-download-make
	echo '# MKHASH is used in /scripts, so we export it here.' >> nf-patch-build/libnftnl-download-make
	echo 'export MKHASH' >> nf-patch-build/libnftnl-download-make
	echo '# DOWNLOAD_CHECK_CERTIFICATE is used in /scripts, so we export it here.' >> nf-patch-build/libnftnl-download-make
	echo 'DOWNLOAD_CHECK_CERTIFICATE:=$(CONFIG_DOWNLOAD_CHECK_CERTIFICATE)' >> nf-patch-build/libnftnl-download-make
	echo 'export DOWNLOAD_CHECK_CERTIFICATE' >> nf-patch-build/libnftnl-download-make
	egrep "CONFIG_LINUX_.*=y" .config | sed 's/=/:=/g' >> nf-patch-build/libnftnl-download-make

	package_include_line_num=$(cat package/libs/libnftnl/Makefile | egrep -n "include.*package.mk" | sed 's/:.*$//g' )
	head -n $package_include_line_num package/libs/libnftnl/Makefile | awk ' { if( ( $0 !~ /^include/ ) && ($0 !~ /^#/ )){ print $0 ; }} ' >> nf-patch-build/libnftnl-download-make

	echo '' >> nf-patch-build/libnftnl-download-make
	echo 'include $(TOPDIR)/include/download.mk' >> nf-patch-build/libnftnl-download-make
	echo '' >> nf-patch-build/libnftnl-download-make
	

	echo 'all:' >> nf-patch-build/libnftnl-download-make
	echo '	if [ ! -e "$(DL_DIR)/$(PKG_SOURCE)" ] ; then  TOPDIR="$(TOPDIR)" $(SCRIPT_DIR)/download.pl $(DL_DIR) $(PKG_SOURCE) $(PKG_HASH)  $(PKG_SOURCE)  $(PKG_SOURCE_URL) ; fi ; ' >> nf-patch-build/libnftnl-download-make
	echo '	cp $(DL_DIR)/$(PKG_SOURCE) . ' >>nf-patch-build/libnftnl-download-make
	echo '	tar xf $(PKG_SOURCE)' >>nf-patch-build/libnftnl-download-make
	echo '	rm -rf *.bz2 *.xz' >>nf-patch-build/libnftnl-download-make
	echo '	mv libnftnl* libnftnl' >>nf-patch-build/libnftnl-download-make
	echo '	mkdir -p $(TOPDIR)/package/libs/libnftnl/patches/' >>nf-patch-build/libnftnl-download-make
	echo '	$(SCRIPT_DIR)/patch-kernel.sh libnftnl $(TOPDIR)/package/libs/libnftnl/patches/' >>nf-patch-build/libnftnl-download-make
	echo '	echo $(TOPDIR)/package/libs/libnftnl/patches/ > libnftnl-patch-dir' >>nf-patch-build/libnftnl-download-make
fi



cd nf-patch-build


####################################################################################################
##### Build Patches  ###############################################################################
####################################################################################################

if [ "$patch_kernel" = 1 ] ; then
	mv linux-download-make Makefile
	make
	mv linux linux.orig
	cp -r linux.orig linux.new


	mv iptables-download-make Makefile
	make
	mv iptables iptables.orig
	cp -r iptables.orig iptables.new
	
	
	mv nftables-download-make Makefile
	make
	mv nftables nftables.orig
	cp -r nftables.orig nftables.new
	
	
	mv libnftnl-download-make Makefile
	make
	mv libnftnl libnftnl.orig
	cp -r libnftnl.orig libnftnl.new


	generic_config_file=$(cat generic-config-file)
	config_file=$(cat config-file)
	patch_dir=$(cat patch-dir)
	iptables_patch_dir=$(cat iptables-patch-dir)
	nftables_patch_dir=$(cat nftables-patch-dir)
	libnftnl_patch_dir=$(cat libnftnl-patch-dir)
	
	mkdir -p "$iptables_patch_dir"
	mkdir -p "$nftables_patch_dir"
	mkdir -p "$libnftnl_patch_dir"
	mkdir -p "$patch_dir"
fi

echo "new_iptables_module_dirs=$new_iptables_module_dirs"
echo "new_nftables_module_dirs=$new_nftables_module_dirs"

for new_d in $new_iptables_module_dirs ; do
	new_d="$iptables_module_dir/$new_d"
	new_name=$(cat $new_d/name 2>/dev/null)
	upper_name=$(echo "$new_name" | tr "[:lower:]" "[:upper:]")
	lower_name=$(echo "$new_name" | tr "[:upper:]" "[:lower:]")
	
	echo "found iptables $upper_name module, patching..."
	
	if [ "$patch_kernel" = 1 ] ; then		
		#copy files for netfilter module
		cp -r $new_d/module/* linux.new/net/netfilter/
		cp -r $new_d/header/* linux.new/include/linux/netfilter/
	
		#update netfilter Makefile
		match_comment_line_num=$(cat linux.new/net/netfilter/Makefile | egrep -n "#.*[Mm][Aa][Tt][Cc][Hh]" | sed 's/:.*$//g' )
		config_line='obj-$(CONFIG_NETFILTER_XT_MATCH_'$upper_name') += xt_'$lower_name'.o' 
		insert_lines_at "$match_comment_line_num" "$config_line" "linux.new/net/netfilter/Makefile" "1"
		cp  "linux.new/net/netfilter/Makefile" ./test1

		#update netfilter Config.in/Kconfig file
		if [ -e linux.new/net/netfilter/Kconfig ] ; then
			end_line_num=$(cat linux.new/net/netfilter/Kconfig | egrep -n "endmenu" | sed 's/:.*$//g' )
			insert_line_num=$(($end_line_num-1))
			config_lines=$(printf "%s\n"  "config NETFILTER_XT_MATCH_$upper_name" "	tristate \"$lower_name match support\"" "	depends on NETFILTER_XTABLES" "	help" "		This option enables $lower_name match support." "" "")
			insert_lines_at "$insert_line_num" "$config_lines" "linux.new/net/netfilter/Kconfig" "1"
		fi
		if [ -e linux.new/net/netfilter/Config.in ] ; then
			match_comment_line_num=$(cat linux.new/net/netfilter/Config.in | egrep -n "#.*[Mm][Aa][Tt][Cc][Hh]" | sed 's/:.*$//g' )
			match_comment_line="  dep_tristate '  $lower_name match support' CONFIG_NETFILTER_XT_MATCH_$upper_name \$CONFIG_NETFILTER_XTABLES"
			insert_lines_at "$match_comment_line_num" "$match_comment_line" "linux.new/net/netfilter/Config.in" "1"
			cp  "linux.new/net/netfilter/Config.in" ./test2
		fi
	
		#copy files for iptables extension
		cp -r $new_d/extension/* iptables.new/extensions
		cp -r $new_d/header/*    iptables.new/include/linux/netfilter/

		#create test file, which is used by iptables Makefile
		echo "#!/bin/sh" > "iptables.new/extensions/.$lower_name-test"
		echo "[ -f \$KERNEL_DIR/include/linux/netfilter/xt_$lower_name.h ] && echo $lower_name" >> "iptables.new/extensions/.$lower_name-test"
		chmod 777 "iptables.new/extensions/.$lower_name-test"

		#update config templates -- just for simplicity do so for both 2.4-generic and 2.6-generic 
		for config in $generic_config_file $config_file ; do
			echo "CONFIG_NETFILTER_XT_MATCH_$upper_name=m" >> $config
		done
	fi
	
	kernel_netfilter_mk="package/kernel/linux/modules/netfilter.mk"
	iptables_makefile="package/network/utils/iptables/Makefile"
	if [ "$patch_openwrt" = "1" ] ; then
		#add OpenWrt package definition for netfilter module
		echo "" >>../"$kernel_netfilter_mk"
		echo "" >>../"$kernel_netfilter_mk"
		echo "define KernelPackage/ipt-$lower_name" >>../"$kernel_netfilter_mk"

		echo "  SUBMENU:=\$(NF_MENU)" >>../"$kernel_netfilter_mk"
		echo "  TITLE:=$lower_name" >>../"$kernel_netfilter_mk"
		echo "  KCONFIG:=\$(KCONFIG_XT_$upper_name)" >>../"$kernel_netfilter_mk"
		echo "  FILES:=\$(LINUX_DIR)/net/netfilter/xt_$lower_name*.\$(LINUX_KMOD_SUFFIX)" >>../"$kernel_netfilter_mk"
		echo "  AUTOLOAD:=\$(call AutoLoad,45,\$(notdir \$(IPT_$upper_name-m)))" >>../"$kernel_netfilter_mk"
		if [ "$lower_name" = "layer7" ] ; then
			echo "	DEPENDS:= +kmod-ipt-core +kmod-ipt-conntrack" >>../"$kernel_netfilter_mk"
		else
			echo "	DEPENDS:= kmod-ipt-core" >>../"$kernel_netfilter_mk"
		fi
		echo "endef" >>../"$kernel_netfilter_mk"
		echo "\$(eval \$(call KernelPackage,ipt-$lower_name))" >>../"$kernel_netfilter_mk"

	
		#add OpenWrt package definition for iptables extension
		echo "" >>../"$iptables_makefile" 
		echo "" >>../"$iptables_makefile" 
		echo "define Package/iptables-mod-$lower_name" >>../"$iptables_makefile" 
		echo "\$(call Package/iptables/Module, +kmod-ipt-$lower_name)" >>../"$iptables_makefile" 
		echo "  TITLE:=$lower_name" >>../"$iptables_makefile" 
		echo "endef" >>../"$iptables_makefile" 
		echo "\$(eval \$(call BuildPlugin,iptables-mod-$lower_name,\$(IPT_$upper_name-m)))" >>../"$iptables_makefile" 
	
	
		#update include/netfilter.mk with new module
		echo "">>../include/netfilter.mk
		echo "">>../include/netfilter.mk
		echo "IPT_$upper_name-m :=">>../include/netfilter.mk
		echo "IPT_$upper_name-\$(CONFIG_NETFILTER_XT_MATCH_$upper_name) += \$(P_XT)xt_$lower_name">>../include/netfilter.mk
		echo "IPT_BUILTIN += \$(IPT_$upper_name-y)">>../include/netfilter.mk
	fi
done

for new_d in $new_nftables_module_dirs ; do
	new_d="$nftables_module_dir/$new_d"
	new_name=$(cat $new_d/name 2>/dev/null)
	upper_name=$(echo "$new_name" | tr "[:lower:]" "[:upper:]")
	lower_name=$(echo "$new_name" | tr "[:upper:]" "[:lower:]")
	
	echo "found nftables $upper_name module, patching..."
	
	if [ "$patch_kernel" = 1 ] ; then		
		#copy files for netfilter module
		cp -r $new_d/module/* linux.new/net/netfilter/
		cp -r $new_d/header/* linux.new/include/linux/netfilter/
	
		#update netfilter Makefile
		match_comment_line_num=$(cat linux.new/net/netfilter/Makefile | egrep -n "obj-\\$\\(CONFIG_NF_TABLES\\).*" | sed 's/:.*$//g' )
		config_line='obj-$(CONFIG_NFT_'$upper_name') += nft_'$lower_name'.o' 
		insert_lines_at "$match_comment_line_num" "$config_line" "linux.new/net/netfilter/Makefile" "1"
		cp  "linux.new/net/netfilter/Makefile" ./test1

		#update netfilter Kconfig file
		if [ -e linux.new/net/netfilter/Kconfig ] ; then
			end_line_num=$(cat linux.new/net/netfilter/Kconfig | egrep -n "endmenu" | sed 's/:.*$//g' )
			insert_line_num=$(($end_line_num-1))
			config_lines=$(printf "%s\n"  "config NFT_$upper_name" "	tristate \"Netfilter nf_tables $lower_name match expression support\"" "	help" "		This option enables $lower_name expression support." "" "")
			insert_lines_at "$insert_line_num" "$config_lines" "linux.new/net/netfilter/Kconfig" "1"
		fi

		#update files for nftables extension
		if [ -e $new_d/nftables/meta ] ; then
			while IFS=$'\n' read -r line;
			do
				echo "$line" | grep -q "^#" && continue
				infile="$(echo "$line" | awk -F '\\|\\|' '{print $1}')"
				action="$(echo "$line" | awk -F '\\|\\|' '{print $2}')"
				if [ "$action" = "copy" ] ; then
					outfile="$(echo "$line" | awk -F '\\|\\|' '{print $3}')"
					
					cp $new_d/nftables/$infile nftables.new/$outfile
				elif [ "$action" = "insert" ] ; then
					direction="$(echo "$line" | awk -F '\\|\\|' '{print $3}')"
					offset="$(echo "$line" | awk -F '\\|\\|' '{print $4}')"
					pat="$(printf '%s' "$line" | awk -F '\\|\\|' '{print $5}')"
					outfile="$(echo "$line" | awk -F '\\|\\|' '{print $6}')"
					
					mult=1
					[ "$direction" = "before" ] && mult=-1
					
					insert_line_num=$(cat nftables.new/$outfile | egrep -n "$pat" | sed 's/:.*$//g' )
					insert_line_num=$(($insert_line_num+($offset*$mult)))
					
					config_lines=$(cat $new_d/nftables/$infile)
					
					insert_lines_at "$insert_line_num" "$config_lines" "nftables.new/$outfile" "1"
				fi
			done < "$new_d/nftables/meta"
		fi

		#update files for libnftnl extension
		if [ -e $new_d/libnftnl/meta ] ; then
			while IFS=$'\n' read -r line;
			do
				echo "$line" | grep -q "^#" && continue
				infile="$(echo "$line" | awk -F '\\|\\|' '{print $1}')"
				action="$(echo "$line" | awk -F '\\|\\|' '{print $2}')"
				if [ "$action" = "copy" ] ; then
					outfile="$(echo "$line" | awk -F '\\|\\|' '{print $3}')"
					
					cp $new_d/libnftnl/$infile libnftnl.new/$outfile
				elif [ "$action" = "insert" ] ; then
					direction="$(echo "$line" | awk -F '\\|\\|' '{print $3}')"
					offset="$(echo "$line" | awk -F '\\|\\|' '{print $4}')"
					pat="$(echo "$line" | awk -F '\\|\\|' '{print $5}')"
					outfile="$(echo "$line" | awk -F '\\|\\|' '{print $6}')"
					
					mult=1
					[ "$direction" = "before" ] && mult=-1
					
					insert_line_num=$(cat libnftnl.new/$outfile | egrep -n "$pat" | sed 's/:.*$//g' )
					insert_line_num=$(($insert_line_num+($offset*$mult)))
					
					config_lines=$(cat $new_d/libnftnl/$infile)
					
					insert_lines_at "$insert_line_num" "$config_lines" "libnftnl.new/$outfile" "1"
				fi
			done < "$new_d/libnftnl/meta"
		fi

		#modify libnftnl src/Makefile.in
		insert_line_num=$(cat libnftnl.new/src/Makefile.in | egrep -n "am_libnftnl_la_OBJECTS = " | sed 's/:.*$//g' )
		insert_line_num=$(($insert_line_num+3))
		config_lines=$(printf "%s"  '	expr/'$lower_name'.lo \\')
		insert_lines_at "$insert_line_num" "$config_lines" "libnftnl.new/src/Makefile.in" "1"
		
		insert_line_num=$(cat libnftnl.new/src/Makefile.in | egrep -n "am__depfiles_remade = " | sed 's/:.*$//g' )
		insert_line_num=$(($insert_line_num+7))
		config_lines=$(printf "%s"  '	expr/$(DEPDIR)/'$lower_name'.Plo \\')
		insert_lines_at "$insert_line_num" "$config_lines" "libnftnl.new/src/Makefile.in" "1"
		
		insert_line_num=$(cat libnftnl.new/src/Makefile.in | egrep -n "libnftnl_la_SOURCES = " | sed 's/:.*$//g' )
		insert_line_num=$(($insert_line_num+1))
		config_lines=$(printf "%s"  '	      expr/'$lower_name'.c \\')
		insert_lines_at "$insert_line_num" "$config_lines" "libnftnl.new/src/Makefile.in" "1"
		
		insert_line_num=$(cat libnftnl.new/src/Makefile.in | egrep -n "expr/xfrm\\.lo: expr/\\$\\(am__dirstamp\\) expr/\\$\\(DEPDIR\\)/\\$\\(am__dirstamp\\)" | sed 's/:.*$//g' )
		insert_line_num=$(($insert_line_num+0))
		config_lines=$(printf "%s\n"  "expr/$lower_name.lo: expr/\$(am__dirstamp) expr/\$(DEPDIR)/\$(am__dirstamp)")
		insert_lines_at "$insert_line_num" "$config_lines" "libnftnl.new/src/Makefile.in" "1"
		
		insert_line_num=$(cat libnftnl.new/src/Makefile.in | egrep -n "@AMDEP_TRUE@@am__include@ @am__quote@expr/\\$\\(DEPDIR\\)/xfrm\\.Plo" | sed 's/:.*$//g' )
		insert_line_num=$(($insert_line_num+0))
		config_lines=$(printf "%s\n"  "@AMDEP_TRUE@@am__include@ @am__quote@expr/\$(DEPDIR)/$lower_name.Plo@am__quote@ # am--include-marker")
		insert_lines_at "$insert_line_num" "$config_lines" "libnftnl.new/src/Makefile.in" "1"
		
		insert_line_num=$(cat libnftnl.new/src/Makefile.in | egrep -n "distclean:" | sed 's/:.*$//g' )
		insert_line_num=$(($insert_line_num+1))
		config_lines=$(printf "%s\n"  "	-rm -f expr/\$(DEPDIR)/$lower_name.Plo")
		insert_lines_at "$insert_line_num" "$config_lines" "libnftnl.new/src/Makefile.in" "1"
		
		insert_line_num=$(cat libnftnl.new/src/Makefile.in | egrep -n "maintainer-clean:" | sed 's/:.*$//g' )
		insert_line_num=$(($insert_line_num+1))
		config_lines=$(printf "%s\n"  "	-rm -f expr/\$(DEPDIR)/$lower_name.Plo")
		insert_lines_at "$insert_line_num" "$config_lines" "libnftnl.new/src/Makefile.in" "1"


		#update config templates -- just for simplicity do so for both 2.4-generic and 2.6-generic 
		for config in $generic_config_file $config_file ; do
			echo "CONFIG_NFT_$upper_name=m" >> $config
		done
	fi
	
	kernel_netfilter_mk="package/kernel/linux/modules/netfilter.mk"
	iptables_makefile="package/network/utils/iptables/Makefile"
	if [ "$patch_openwrt" = "1" ] ; then
		#add OpenWrt package definition for netfilter module
		echo "" >>../"$kernel_netfilter_mk"
		echo "" >>../"$kernel_netfilter_mk"
		echo "define KernelPackage/nft-$lower_name" >>../"$kernel_netfilter_mk"

		echo "  SUBMENU:=\$(NF_MENU)" >>../"$kernel_netfilter_mk"
		echo "  TITLE:=$lower_name" >>../"$kernel_netfilter_mk"
		echo "  KCONFIG:=\$(KCONFIG_NFT_$upper_name)" >>../"$kernel_netfilter_mk"
		echo "  FILES:=\$(LINUX_DIR)/net/netfilter/nft_$lower_name*.\$(LINUX_KMOD_SUFFIX)" >>../"$kernel_netfilter_mk"
		echo "  AUTOLOAD:=\$(call AutoLoad,45,\$(notdir \$(NFT_$upper_name-m)))" >>../"$kernel_netfilter_mk"
		echo "	DEPENDS:= kmod-nft-core" >>../"$kernel_netfilter_mk"
		echo "endef" >>../"$kernel_netfilter_mk"
		echo "\$(eval \$(call KernelPackage,nft-$lower_name))" >>../"$kernel_netfilter_mk"	
	
		#update include/netfilter.mk with new module
		echo "">>../include/netfilter.mk
		echo "">>../include/netfilter.mk
		echo "\$(eval \$(if \$(NF_KMOD),\$(call nf_add,NFT_$upper_name,CONFIG_NFT_$upper_name, \$(P_XT)nft_$lower_name),))">>../include/netfilter.mk
	fi
done

if [ "$patch_kernel" = 1 ] ; then	
	#build netfilter patch file
	rm -rf $patch_dir/650-custom_netfilter_match_modules.patch 2>/dev/null
	cd linux.new
	module_files=$(find net/netfilter)
	include_files=$(find include/linux/netfilter)
	test_files="$module_files $include_files"
	cd ..
	for t in $test_files ; do
		if [ ! -d "linux.new/$t" ] ; then
			if [ -e "linux.orig/$t" ] ; then
				diff -u "linux.orig/$t" "linux.new/$t" >> $patch_dir/650-custom_netfilter_match_modules.patch
			else
				diff -u /dev/null "linux.new/$t" >> $patch_dir/650-custom_netfilter_match_modules.patch
			fi	
		fi
	done

	#build iptables patch file
	rm -f ../package/iptables/patches/650-custom_netfilter_match_modules.patch 2>/dev/null
	cd iptables.new
	extension_files=$(find extensions)
	include_files=$(find include/linux/netfilter)
	cd ..
	for t in $extension_files $include_files ; do
		if [ ! -d "iptables.new/$t" ] ; then
			if [ -e "iptables.orig/$t" ] ; then
				diff -u "iptables.orig/$t" "iptables.new/$t" >>$iptables_patch_dir/650-custom_netfilter_match_modules.patch
			else
				diff -u /dev/null "iptables.new/$t" >>$iptables_patch_dir/650-custom_netfilter_match_modules.patch 
			fi
		fi	
	done
	
	#build nftables patch file
	rm -f ../package/nftables/patches/650-custom_netfilter_match_modules.patch 2>/dev/null
	cd nftables.new
	src_files=$(find src)
	include_files=$(find include)
	cd ..
	for t in $src_files $include_files ; do
		if [ ! -d "nftables.new/$t" ] ; then
			if [ -e "nftables.orig/$t" ] ; then
				diff -u "nftables.orig/$t" "nftables.new/$t" >>$nftables_patch_dir/650-custom_netfilter_match_modules.patch
			else
				diff -u /dev/null "nftables.new/$t" >>$nftables_patch_dir/650-custom_netfilter_match_modules.patch 
			fi
		fi
	done
	
	#build libnftnl patch file
	rm -f ../package/nftables/patches/650-custom_netfilter_match_modules.patch 2>/dev/null
	cd libnftnl.new
	src_files=$(find src)
	include_files=$(find include)
	cd ..
	for t in $src_files $include_files ; do
		if [ ! -d "libnftnl.new/$t" ] ; then
			if [ -e "libnftnl.orig/$t" ] ; then
				diff -u "libnftnl.orig/$t" "libnftnl.new/$t" >>$libnftnl_patch_dir/650-custom_netfilter_match_modules.patch
			else
				diff -u /dev/null "libnftnl.new/$t" >>$libnftnl_patch_dir/650-custom_netfilter_match_modules.patch 
			fi
		fi
	done
fi

#cleanup
cd ..

rm -rf nf-patch-build

