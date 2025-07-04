include $(TOPDIR)/rules.mk

PKG_NAME:=gargoyle-firewall-util
PKG_VERSION:=$(GARGOYLE_VERSION)
ifeq ($(GARGOYLE_VERSION),)
     PKG_VERSION:=1.0.0
endif
PKG_RELEASE:=1

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/gargoyle-firewall-util
	SECTION:=net
	CATEGORY:=Network
	TITLE:=A couple of shell script routines for firewall (fw4) initialization
	DEPENDS:=+nftables-json +libericstools +uci +libnftbwctl +kmod-nft-bandwidth +kmod-nft-timerange +kmod-nft-weburl +kmod-tun +kmod-nf-nathelper +kmod-nf-nathelper-extra
	MAINTAINER:=Eric Bishop <eric@gargoyle-router.com>
endef

define Package/gargoyle-firewall-util/description
	A couple of shell script routines for firewall initialization
endef

define Build/Prepare
	echo PACKAGE BUILD DIR = $(PACKAGE_BUILD_DIR)
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef


define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		$(TARGET_CONFIGURE_OPTS) \
		STAGING_DIR="$(STAGING_DIR)" \
		CFLAGS="$(TARGET_CFLAGS) -I $(STAGING_DIR)/usr/include" \
		LDFLAGS="$(TARGET_LDFLAGS) -L $(STAGING_DIR)/usr/lib" 
endef



define Package/gargoyle-firewall-util/postinst
	included=$$(cat $${IPKG_INSTROOT}/etc/config/firewall | grep 'gargoyle_additions.firewall'  )
	if [ -z "$$included" ] ; then printf "config include\n\toption type script\n\toption path /usr/lib/gargoyle_firewall_util/gargoyle_additions.firewall\n\n" >> $${IPKG_INSTROOT}/etc/config/firewall ; fi
endef

define Package/gargoyle-firewall-util/install
	$(INSTALL_DIR) $(1)/usr/lib/gargoyle_firewall_util/
	$(INSTALL_DIR) $(1)/etc/hotplug.d/iface/
	$(INSTALL_DIR) $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/init.d/
	$(INSTALL_DIR) $(1)/etc/ppp/ip-up.d/
	$(INSTALL_DIR) $(1)/etc/uci-defaults/
	
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/make_nftables_rules $(1)/usr/bin/make_nftables_rules
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/delete_chain_from_table $(1)/usr/bin/delete_chain_from_table
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/backup_quotas $(1)/usr/bin/backup_quotas
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/restore_quotas $(1)/usr/bin/restore_quotas
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/print_quotas $(1)/usr/bin/print_quotas
	
	$(INSTALL_BIN) ./files/cache_dhcpv4_leases.sh $(1)/usr/lib/gargoyle_firewall_util/cache_dhcpv4_leases.sh
	$(INSTALL_BIN) ./files/gargoyle_firewall_util.sh $(1)/usr/lib/gargoyle_firewall_util/gargoyle_firewall_util.sh
	$(INSTALL_BIN) ./files/gargoyle_additions.firewall $(1)/usr/lib/gargoyle_firewall_util/gargoyle_additions.firewall 
	$(INSTALL_BIN) ./files/gargoyle_firewall.hotplug $(1)/etc/hotplug.d/iface/21-gargoyle_firewall
	$(INSTALL_BIN) ./files/set_kernel_timezone.init $(1)/etc/init.d/set_kernel_timezone
	$(INSTALL_BIN) ./files/modemaccess.pppoe $(1)/etc/ppp/ip-up.d/modemaccess.sh
	$(INSTALL_BIN) ./files/gargoyle_firewall.ucidefaults $(1)/etc/uci-defaults/40-gargoyle-firewall
endef


$(eval $(call BuildPackage,gargoyle-firewall-util))
