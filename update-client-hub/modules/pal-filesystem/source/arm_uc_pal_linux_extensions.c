// ----------------------------------------------------------------------------
// Copyright 2016-2017 ARM Ltd.
//
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ----------------------------------------------------------------------------

#include "update-client-common/arm_uc_config.h"
#if defined(ARM_UC_FEATURE_PAL_FILESYSTEM) && (ARM_UC_FEATURE_PAL_FILESYSTEM == 1)
#if defined(TARGET_IS_PC_LINUX)

#include "update-client-pal-filesystem/arm_uc_pal_extensions.h"

#include "update-client-metadata-header/arm_uc_metadata_header_v2.h"
#include "arm_uc_pal_filesystem_utils.h"

#include "pal.h"

#include "mbed-trace/mbed_trace.h"
#define TRACE_GROUP "update-client-extensions"

#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

#ifndef MBED_CONF_UPDATE_CLIENT_APPLICATION_DETAILS
#define MBED_CONF_UPDATE_CLIENT_APPLICATION_DETAILS 0
#endif

#ifndef MBED_CONF_UPDATE_CLIENT_BOOTLOADER_DETAILS
#define MBED_CONF_UPDATE_CLIENT_BOOTLOADER_DETAILS 0
#endif

static void (*arm_ucex_linux_callback)(uintptr_t) = NULL;
static palImageId_t arm_ucex_activate_image_id;

#ifndef PAL_UPDATE_ACTIVATE_SCRIPT
#define PAL_UPDATE_ACTIVATE_SCRIPT "./activate_script"
#endif

// IMAGE_HEADER_FILENAME_UPDATE points to filename/path where the
// active firmware metadata header is to be found.
// At the end of update the above activate-script should copy the
// new metadata header to this path so that new version
// gets reported to the cloud in next bootup
#define IMAGE_HEADER_FILENAME_UPDATE    "header.bin"

arm_uc_error_t pal_ext_imageInitAPI(void (*callback)(uintptr_t))
{
    arm_uc_error_t result = { .code = ERR_NONE };

    arm_ucex_linux_callback = callback;

    return result;
}

arm_uc_error_t pal_ext_imageGetActiveDetails(arm_uc_firmware_details_t *details)
{
    arm_uc_error_t result = { .code = ERR_INVALID_PARAMETER };

    if (details) {
        palFileDescriptor_t fd;
        palStatus_t status = pal_fsFopen(IMAGE_HEADER_FILENAME_UPDATE, PAL_FS_FLAG_READONLY, &fd);
        if (PAL_SUCCESS == status) {
            uint8_t read_buffer[ARM_UC_EXTERNAL_HEADER_SIZE_V2];
            size_t bytes_read;
            status = pal_fsFread(&fd, &read_buffer, sizeof(read_buffer), &bytes_read);
            if (PAL_SUCCESS == status) {
                /* read out header magic */
                uint32_t headerMagic = arm_uc_parse_uint32(&read_buffer[0]);

                /* read out header magic */
                uint32_t headerVersion = arm_uc_parse_uint32(&read_buffer[4]);

                /* choose version to decode */
                if (headerMagic == ARM_UC_INTERNAL_HEADER_MAGIC_V2 &&
                        headerVersion == ARM_UC_INTERNAL_HEADER_VERSION_V2 &&
                        bytes_read == ARM_UC_INTERNAL_HEADER_SIZE_V2) {
                    result = arm_uc_parse_internal_header_v2(read_buffer, details);
                } else if (headerMagic == ARM_UC_EXTERNAL_HEADER_MAGIC_V2 &&
                           headerVersion == ARM_UC_EXTERNAL_HEADER_VERSION_V2 &&
                           bytes_read == ARM_UC_EXTERNAL_HEADER_SIZE_V2) {
                    result = arm_uc_parse_external_header_v2(read_buffer, details);
                } else {
                    /* invalid header format */
                    tr_err("Unrecognized firmware header: magic = 0x%" PRIx32 ", version = 0x%" PRIx32 ", size = %" PRIu32 ,
                           headerMagic, headerVersion, bytes_read);
                }
            }
            pal_fsFclose(&fd);
        } else {
            // XXX TODO: Need to implement version query before any update has been processed.
            //           In this version info is fetched only from header file which is created
            //           during update process.
            tr_err("No image header! pal_fsOpen returned status = %" PRIu32, status);
        }

        if (PAL_SUCCESS != status || ERR_NONE != result.code) {
            // Zero the details
            memset(details, 0, sizeof(arm_uc_firmware_details_t));
        }

        if (arm_ucex_linux_callback) {
            arm_ucex_linux_callback(ARM_UC_PAAL_EVENT_GET_ACTIVE_FIRMWARE_DETAILS_DONE);
        }
    }

    return result;
}

arm_uc_error_t pal_ext_installerGetDetails(arm_uc_installer_details_t *details)
{
    arm_uc_error_t result = { .code = ERR_INVALID_PARAMETER };

    if (details) {
        /* dummy implementation, return 0 */
        memset(details, 0, sizeof(arm_uc_installer_details_t));

        result.code = ERR_NONE;

        if (arm_ucex_linux_callback) {
            arm_ucex_linux_callback(ARM_UC_PAAL_EVENT_GET_INSTALLER_DETAILS_DONE);
        }
    }

    return result;
}

static void pal_ext_imageActivationWorker(const void *location)
{
    char cmd_buf[sizeof(PAL_UPDATE_ACTIVATE_SCRIPT) + 1 + PAL_MAX_FILE_AND_FOLDER_LENGTH + 1];
    char path_buf[PAL_MAX_FILE_AND_FOLDER_LENGTH];

    arm_uc_error_t result = arm_uc_pal_filesystem_get_path(*(palImageId_t *)location, FIRMWARE_IMAGE_ITEM_DATA,
                                                           path_buf, PAL_MAX_FILE_AND_FOLDER_LENGTH);
    palStatus_t rc = PAL_ERR_GENERIC_FAILURE;

    if (result.code == ERR_NONE) {
        int err = snprintf(cmd_buf, sizeof(cmd_buf), "%s %s",
                           PAL_UPDATE_ACTIVATE_SCRIPT, path_buf);
        if (err > 0) {
            rc = PAL_SUCCESS;
        } else {
            tr_err("snprintf failed with err %i", err);
            rc = PAL_ERR_GENERIC_FAILURE;
        }
    }


    if (rc == PAL_SUCCESS) {
        tr_debug("Activate by executing %s", cmd_buf);
        int err = system(cmd_buf);
        err = WEXITSTATUS(err);

        if (err != -1) {
            tr_debug("Activate completed with %" PRId32, err);
            rc = PAL_SUCCESS;
        } else {
            tr_err("system call failed with err %" PRId32, err);
            rc = PAL_ERR_GENERIC_FAILURE;
        }
    }
    fflush(stdout);
    sleep(1);

    if (arm_ucex_linux_callback) {
        uint32_t event = (rc == PAL_SUCCESS ? ARM_UC_PAAL_EVENT_ACTIVATE_DONE : ARM_UC_PAAL_EVENT_ACTIVATE_ERROR);
        arm_ucex_linux_callback(event);
    }
}

arm_uc_error_t pal_ext_imageActivate(uint32_t location)
{
    arm_uc_error_t err = { .code = ERR_INVALID_PARAMETER };

    memcpy(&arm_ucex_activate_image_id, &location, sizeof(palImageId_t));

    palThreadID_t thread_id = 0;
    palStatus_t rc = pal_osThreadCreateWithAlloc(pal_ext_imageActivationWorker, &arm_ucex_activate_image_id,
                                                 PAL_osPriorityBelowNormal, PTHREAD_STACK_MIN, NULL, &thread_id);
    if (rc != PAL_SUCCESS) {
        tr_err("Thread creation failed with %x", rc);
        err.code = ERR_INVALID_PARAMETER;
    } else {
        tr_debug("Activation thread created, thread ID: %" PRIuPTR, thread_id);
        err.code = ERR_NONE;
    }

    return err;
}

#endif /* TARGET_IS_PC_LINUX */
#endif /* ARM_UC_FEATURE_PAL_FILESYSTEM */
