include $(TOPDIR)/rules.mk

PKG_NAME:=gargoyle-profiles
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

# package groups for profiles
GARGOYLE_PKGS_CORE:= +gargoyle +gargoyle-i18n +plugin-gargoyle-i18n-English-EN \
		+qos-gargoyle +plugin-gargoyle-qos \
		+ddns-gargoyle +plugin-gargoyle-ddns \
		+plugin-gargoyle-wol \
		+ewget +relayd +wpad-mbedtls \
		+kmod-nf-nat6 +usteer

GARGOYLE_PKGS_USB:= +plugin-gargoyle-usb-storage-extroot

GARGOYLE_PKGS_VPN:= +plugin-gargoyle-openvpn +openvpn-gargoyle-easy-rsa \
		+wireguard-tools +plugin-gargoyle-wireguard

GARGOYLE_PKGS_LARGE:= +plugin-gargoyle-usb-storage-full +plugin-gargoyle-minidlna \
		+plugin-gargoyle-usb-printer +plugin-gargoyle-webcam \
		+plugin-gargoyle-ipheth-tether

define Package/gargoyle-profiles/Default
	CATEGORY:=Administration
	SUBMENU:=Gargoyle Profile Meta-packages
	SECTION:=admin
	TITLE:=Gargoyle device profile -
	MAINTAINER:=Eric Bishop <eric@gargoyle-router.com>
	DEPENDS:= $(GARGOYLE_PKGS_CORE)
endef

define Package/gargoyle-profiles/description/Default
 A user-friendly web interface for OpenWrt
endef

define Package/gargoyle-basic
	$(call Package/gargoyle-profiles/Default)
	TITLE+= basic functionality
endef

define Package/gargoyle-basic/description
	$(call Package/gargoyle-profiles/description/Default)
	.
	This meta package contains only the dependencies required for
	basic Gargoyle functionality.
endef

define Package/gargoyle-basic/install
	/bin/true
endef

define Package/gargoyle-usb
	$(call Package/gargoyle-profiles/Default)
	TITLE+= basic functionality, USB local storage, USB networking
	DEPENDS+= $(GARGOYLE_PKGS_USB)
endef

define Package/gargoyle-usb/description
	$(call Package/gargoyle-profiles/description/Default)
	.
	This meta package contains only the dependencies required for 
	basic Gargoyle functionality plus USB local storage and USB
	network devices.
endef

define Package/gargoyle-usb/install
	/bin/true
endef

define Package/gargoyle-vpn
	$(call Package/gargoyle-profiles/Default)
	TITLE+= basic functionality, VPN support
	DEPENDS+= $(GARGOYLE_PKGS_VPN)
endef

define Package/gargoyle-vpn/description
	$(call Package/gargoyle-profiles/description/Default)
	.
	This meta package contains only the dependencies required for 
	basic Gargoyle functionality plus VPN support.
endef

define Package/gargoyle-vpn/install
	/bin/true
endef

define Package/gargoyle-large
	$(call Package/gargoyle-profiles/Default)
	TITLE+= full functionality
	DEPENDS+= $(GARGOYLE_PKGS_VPN) $(GARGOYLE_PKGS_LARGE)
endef

define Package/gargoyle-large/description
	$(call Package/gargoyle-profiles/description/Default)
	.
	This meta package contains only the dependencies required for 
	full Gargoyle functionality.
endef

define Package/gargoyle-large/install
	/bin/true
endef

define Build/Compile
endef

define Build/Configure
endef

$(eval $(call BuildPackage,gargoyle-basic))
$(eval $(call BuildPackage,gargoyle-usb))
$(eval $(call BuildPackage,gargoyle-vpn))
$(eval $(call BuildPackage,gargoyle-large))
