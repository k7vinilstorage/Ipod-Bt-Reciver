// BTstack configuration for a2dpsinkd (iPod touch 5 / Broadcom BCM4334, Classic only)
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// Port related features
#define HAVE_ASSERT
#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_POSIX_TIME

// BTstack features that can be enabled
#define ENABLE_CLASSIC
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP

// Broadcom controllers on iOS use 1021 byte ACL buffers
#define HCI_ACL_PAYLOAD_SIZE 1021

// Remember this many paired phones
#define NVM_NUM_LINK_KEYS 16

#endif
