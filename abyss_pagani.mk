#
# Copyright (C) 2021-2025 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# Inherit from pagani device
$(call inherit-product, device/oneplus/pagani/device.mk)

# Inherit some common Lineage stuff.
$(call inherit-product, vendor/abyss/config/common_full_phone.mk)

PRODUCT_NAME := abyss_pagani
PRODUCT_DEVICE := pagani
PRODUCT_MANUFACTURER := OnePlus
PRODUCT_BRAND := OnePlus
PRODUCT_MODEL := PKX110

PRODUCT_GMS_CLIENTID_BASE := android-oneplus

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc="qssi_64-user 16 AP3A.240617.008 1761797411 release-keys" \
    BuildFingerprint=OnePlus/PKX110/OP60F5L1:16/AP3A.240617.008/V.192ddd9_77f7b3_768d04:user/release-keys \
    DeviceName=OP60F5L1 \
    DeviceProduct=PKX110 \
    SystemDevice=OP60F5L1 \
    SystemName=PKX110