/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Generated version-one USB protocol identifiers and envelope limits for
 * ESP-IDF firmware. Regenerate with protocol/schema/generate-header.ps1. */
#ifndef ESP32_KVM_PROTOCOL_V1_H
#define ESP32_KVM_PROTOCOL_V1_H
#define KVM_PROTOCOL_MAGIC 0x4B56u
#define KVM_PROTOCOL_MAJOR 1u
#define KVM_PROTOCOL_HEADER_LEN 24u
#define KVM_PROTOCOL_MAX_PAYLOAD 512u
#define KVM_PROTOCOL_MAX_FRAME 540u
#define KVM_MSG_HELLO 0x01u
#define KVM_MSG_CAPS 0x02u
#define KVM_MSG_SESSION_OPEN 0x03u
#define KVM_MSG_HEARTBEAT 0x10u
#define KVM_MSG_GET_STATUS 0x11u
#define KVM_MSG_STATUS 0x12u
#define KVM_MSG_SWITCH 0x20u
#define KVM_MSG_RELEASE_ALL 0x21u
#define KVM_MSG_ARM 0x22u
#define KVM_MSG_KEY_STATE 0x30u
#define KVM_MSG_POINTER 0x31u
#define KVM_MSG_CONSUMER_STATE 0x32u
#define KVM_MSG_PAIR_BEGIN 0x40u
#define KVM_MSG_PAIR_CANCEL 0x41u
#define KVM_MSG_FORGET_BOND 0x42u
#define KVM_MSG_PAIR_REPLY 0x43u
#define KVM_MSG_DEVICE_SELECT_REQUEST 0x50u
#define KVM_MSG_UPDATE_PREPARE 0x60u
#define KVM_MSG_ACK 0x70u
#define KVM_MSG_NACK 0x71u
#define KVM_MSG_INPUT_PROGRESS 0x72u
#endif /* ESP32_KVM_PROTOCOL_V1_H */
