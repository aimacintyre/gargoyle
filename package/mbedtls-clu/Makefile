include $(TOPDIR)/rules.mk

PKG_NAME:=mbedtls-clu
PKG_SOURCE_DATE:=2026-08-25
PKG_SOURCE_VERSION:=c4b6195872896d8b32c9b0607066ca47fbbe8385
PKG_VERSION:=1.2.0
PKG_RELEASE:=1

MAKE_PATH:=src

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL=https://github.com/lantis1008/mbedtls-clu.git
PKG_MIRROR_HASH:=007aab73d874eb4b59e216f096da79cd985f0611dfb80d3880c09a75ad5cd32c

# Set to 1 to dynamically link libericstools, or 0 to static compile
DLINK_LIBERICSTOOLS:=1

include $(INCLUDE_DIR)/package.mk

ifeq (1,$(DLINK_LIBERICSTOOLS))
	LIBDEPENDS=+libericstools
else
	MAKE_FLAGS += \
		STATIC_LIBS="1"
endif

define Package/mbedtls-clu
	SECTION:=utils
	CATEGORY:=Utilities
	DEPENDS:=+libmbedtls $(LIBDEPENDS)
	TITLE:=MbedTLS CLU
	MAINTAINER:=Michael Gray <support@lantisproject.com>
endef

define Package/mbedtls-clu/description
	This package contains the MbedTLS Command-Line Utility.
endef

define Package/mbedtls-clu/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/src/mbedtls-clu $(1)/usr/bin/mbedtls
endef

$(eval $(call BuildPackage,mbedtls-clu))
