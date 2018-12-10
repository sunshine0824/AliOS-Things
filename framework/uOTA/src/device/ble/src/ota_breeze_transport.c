#include "ais_ota.h"
#include "ota_breeze_transport.h"
#include "ota_breeze.h"
#include "ota_log.h"

static uint32_t ota_breeze_g_page_erase_total = 0;
static uint32_t ota_breeze_g_page_erase_already = 0;
static uint8_t ota_breeze_new_fw = 0;
extern _ota_ble_global_dat_t g_ctx;

static ota_breeze_bin_info_t ota_breeze_bin_info[] = {
    { OTA_BREEZE_BIN_TYPE_APP,    OTA_BREEZE_BIN_TYPE_MAGIC_APP},
    { OTA_BREEZE_BIN_TYPE_KERNEL, OTA_BREEZE_BIN_TYPE_MAGIC_KERNEL},
    { OTA_BREEZE_BIN_TYPE_SINGLE, OTA_BREEZE_BIN_TYPE_MAGIC_SINGLE},
};

/*
 * <TODO>
 * EXTRACT_U16 and EXTRACT_U32 should be platform dependent.
 * Data, from BLE netmwork is little endian, should be converted
 * to host endian where needed by using these macros.
 */
#define EXTRACT_U16(d) (*((uint8_t *)(d)) | (*((uint8_t *)(d) + 1) << 8))
#define EXTRACT_U32(d)                                  \
    (*((uint8_t *)(d)) | (*((uint8_t *)(d) + 1) << 8) | \
    (*((uint8_t *)(d) + 2) << 16) | (*((uint8_t *)(d) + 3) << 24))


#define ENCODE_U16(d, val)                           \
{                                                \
    *((uint8_t *)(d))     = (val)&0xFF;          \
    *((uint8_t *)(d) + 1) = ((val) >> 8) & 0xFF; \
}

#define ENCODE_U32(d, val)                            \
{                                                 \
    *((uint8_t *)(d))     = (val)&0xFF;           \
    *((uint8_t *)(d) + 1) = ((val) >> 8) & 0xFF;  \
    *((uint8_t *)(d) + 2) = ((val) >> 16) & 0xFF; \
    *((uint8_t *)(d) + 3) = ((val) >> 24) & 0xFF; \
}

/* Poly: 1021, init value: 0xFFFF */
static uint16_t ota_utils_crc16(uint8_t const *p_data, uint32_t size)
{
    uint16_t crc = 0xFFFF;

    for (uint32_t i = 0; i < size; i++) {
        crc = (uint8_t)(crc >> 8) | (crc << 8);
        crc ^= p_data[i];
        crc ^= (uint8_t)(crc & 0xFF) >> 4;
        crc ^= (crc << 8) << 4;
        crc ^= ((crc & 0xFF) << 4) << 1;
    }

    return crc;
}

static uint32_t ota_utils_crc32(uint8_t const *p_data, uint32_t size)
{
    uint32_t crc;

    crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < size; i++) {
        crc = crc ^ p_data[i];
        for (uint32_t j = 8; j > 0; j--) {
            crc = (crc >> 1) ^ (0xEDB88320U & ((crc & 1) ? 0xFFFFFFFF : 0));
        }
    }
    return ~crc;
}

static uint16_t crc16_compute(uint8_t const *add, uint32_t size, void *p)
{
    return ota_utils_crc16(add, size);
}

static uint32_t crc32_compute(uint8_t const *add, uint32_t size, void *p)
{
    return ota_utils_crc32((uint8_t *)add, size);
}


/**@brief Send 0x0F, ERROR. */
void ota_breeze_send_error()
{
    uint32_t err_code = 0;

    err_code = breeze_post_ext( OTA_BREEZE_CMD_ERROR, NULL, 0);
    if (err_code != OTA_BREEZE_SUCCESS) {
        OTA_LOG_E("ota breeze send error failed");
    }
}

/**@brief Send 0x21, FW Version RSP.*/
uint32_t ota_breeze_send_fw_version_rsp(uint8_t ota_cmd, uint8_t *buffer, uint32_t length)
{
    if((buffer == NULL) || (length == 0)) {
        OTA_LOG_E("ota breeze send fw version, input parameters error!");
        return OTA_BREEZE_ERROR_INVALID_PARAM;
    }
    return breeze_post_ext(OTA_BREEZE_CMD_FW_VERSION_RSP, buffer, length);
}

/**@brief Send 0x23, FW upgrade response. */
static uint32_t ota_breeze_send_fw_upgrade_rsp(uint8_t allow_upgrade)
{
    uint32_t err_code;
    uint8_t tx_buf[2] = {0, 0};
    tx_buf[0] = (allow_upgrade) ? 1 : 0;
    err_code = breeze_post_ext(OTA_BREEZE_CMD_FW_UPGRADE_RSP, tx_buf, 1);
    return err_code;
}


/**@brief Send 0x24, number of bytes received. */
static void ota_breeze_send_bytes_received()
{
    uint32_t err_code;
    uint8_t tx_buff[16];
    _ota_ble_global_dat_t* p_ota = &g_ctx;
    
    memset(tx_buff, 0x00, sizeof(tx_buff));
    ENCODE_U16(tx_buff, p_ota->frames_recvd);
    ENCODE_U32(tx_buff + sizeof(uint16_t), p_ota->bytes_recvd);

    err_code = breeze_post_ext(OTA_BREEZE_CMD_FW_BYTES_RECEIVED, tx_buff, sizeof(uint16_t) + sizeof(uint32_t));
    if (err_code != OTA_BREEZE_SUCCESS) {
        OTA_LOG_E("ota breeze send bytes recvd failed");
    }
    return err_code;
}

/**@brief Send 0x25, CRC check result. */
static uint32_t ota_breeze_send_crc_result(uint8_t crc_ok)
{
    uint32_t err_code;
    uint8_t tx_buff[2] = {0x00, 0x00};
    tx_buff[0] = (crc_ok) ? 1 : 0;
    err_code = breeze_post_ext(OTA_BREEZE_CMD_FW_CHECK_RESULT, tx_buff, 1);
    if (err_code != OTA_BREEZE_SUCCESS) {
        OTA_LOG_E("ota breeze send crc result failed");
    }
    return err_code;
}

/**@brief Send 0x26, "New firmware".*/
static void ota_breeze_send_fwup_success()
{
    uint32_t err_code;
    uint8_t tx_buf[2] = {0x00, 0x01};
    err_code = breeze_post_ext(OTA_BREEZE_CMD_FW_UPDATE_PROCESS, tx_buf, 1);
    if (err_code != OTA_BREEZE_SUCCESS) {
        OTA_LOG_E("ota breeze send fwup failed");
    }
}


static uint32_t ota_breeze_check_upgrade_fw_version(ota_breeze_version_t *version, uint8_t *p_data, uint8_t length)
{

    uint8_t v_old[3], v_new[3], l_len, l_part_num;
    uint8_t  l_data_old[OTA_BREEZE_FW_VER_LEN + 1];
    uint8_t  l_data_new[OTA_BREEZE_FW_VER_LEN + 1];
    uint32_t ret = 0;

    if((version == NULL) || (p_data == NULL) || (length == 0)) {
        return OTA_BREEZE_ERROR_INVALID_PARAM;
    }
    // Copy to stack variable as trailing zero is required.
    memcpy(l_data_old, version->fw_ver, version->fw_ver_len);
    l_data_old[version->fw_ver_len] = 0;

    l_len = length - sizeof(uint32_t) - sizeof(uint16_t) - 1;
    memcpy(l_data_new, p_data, l_len);
    l_data_new[l_len] = 0;

    l_part_num = sscanf((char*)l_data_old, "%d.%d.%d", v_old, v_old + 1, v_old + 2);
    sscanf((char*)l_data_new, "%d.%d.%d", v_new, v_new + 1, v_new + 2);
    for(uint8_t i= 0; i < l_part_num; i++){
        if(v_new[i] > v_old[i]){
	    ret = 1;
	    break;
	} else if(v_new[i] < v_old[i]){
	   ret = -1;
	   break;
	}
    }

    if(ret = 1){
        return OTA_BREEZE_SUCCESS;
    } else{
        return OTA_BREEZE_ERROR_FORBIDDEN;
    }
}

static void ota_breeze_disconnect()
{
    /* still have data feedback to app, so do disconnection after a * while */
    ota_msleep(2000);
    breeze_disconnect_ble();
}

void ota_breeze_err_hdl(uint32_t err_code)
{
    extern void ota_breeze_destroy_receive_buf(void);
    ota_breeze_destroy_receive_buf();
    g_ctx.ota_breeze_task_active_flag = false;
}

/**@brief Align address to ceil(page_size), where page_size is 2 to the integer
 * power. */
static uint32_t ota_breeze_align_to_page(uint32_t val, uint32_t page_size)
{
    return ((val + page_size - 1) & ~(page_size - 1));
}


/**@brief Callback function for bootloader settings. */
static void ota_breeze_bootloader_settings_event_handler(ota_breeze_flash_evt_t event)
{
    _ota_ble_global_dat_t* p_ota = &g_ctx;

    switch (p_ota->ota_breeze_status) {
        case OTA_BREEZE_STATE_UPGRADE_REPORT:
            if (OTA_BREEZE_FLASH_STORE_OK == event) {
                p_ota->ota_breeze_status = (p_ota->feature_enable) ? OTA_BREEZE_STATE_IDLE
                                                       : OTA_BREEZE_STATE_OFF;
                ota_breeze_send_fwup_success();
            } else {
                OTA_LOG_E("ota breeze upgrade report:flash store failed");
                return;
            }
            break;

        case OTA_BREEZE_STATE_WRITE_SETTINGS:
            if (OTA_BREEZE_FLASH_STORE_OK == event) {
                p_ota->ota_breeze_status = (p_ota->bytes_recvd >= p_ota->rx_fw_size)
                                 ? OTA_BREEZE_STATE_FW_CHECK
                                 : OTA_BREEZE_STATE_RECEIVE;
                ota_breeze_send_bytes_received();
            } else {
                OTA_LOG_E("ota breeze write settings: flash store failed");
                return;
            }
            break;

        case OTA_BREEZE_STATE_RESET_PREPARE:
            ota_breeze_send_crc_result(true);
            OTA_LOG_I("breeze ota crc report over!!!");
            break;

        default:
            break;
    }
}

/**@brief Callback function for flash write/erase operations. */
static void ota_breeze_flash_event_handler(ota_breeze_flash_evt_t event)
{
    uint32_t err_code = OTA_BREEZE_SUCCESS;
    _ota_ble_global_dat_t* p_ota = &g_ctx;

    switch (p_ota->ota_breeze_status) {
        case OTA_BREEZE_STATE_IDLE:
            if (OTA_BREEZE_FLASH_ERASE_OK == event) {
                err_code = ota_breeze_send_fw_upgrade_rsp(true);
                if (err_code == OTA_BREEZE_SUCCESS) {
                    p_ota->ota_breeze_status = OTA_BREEZE_STATE_RECEIVE;
                }
            }
            else {
                OTA_LOG_E("ota breeze in idle: flash event error");
                return;
            }
            break;

        case OTA_BREEZE_STATE_WRITE:
            if (OTA_BREEZE_FLASH_STORE_OK == event) {
                bool update_bldr_settings = false;

                if (p_ota->bytes_recvd == p_ota->rx_fw_size) {
                    ais_ota_set_setting_fw_offset(p_ota->bytes_recvd);
                    update_bldr_settings = true;
                }
                else if (p_ota->bytes_recvd >=
                           ais_ota_get_setting_fw_offset() +
                             ais_ota_get_page_size()) {
                    ais_ota_set_setting_fw_offset(
                      ais_ota_get_setting_fw_offset() +
                      ais_ota_get_page_size());
                    update_bldr_settings = true;
                }

                if (update_bldr_settings) {
                    err_code = ais_ota_settings_write(
                      /*bootloader_settings_event_handler*/ NULL);
                    if (err_code != ALI_OTA_SETTINGS_CODE_SUCCESS) {
                        OTA_LOG_E("ota breeze setting write failed");
                        return;
                    }
                    p_ota->ota_breeze_status = OTA_BREEZE_STATE_WRITE_SETTINGS;
                    ota_breeze_bootloader_settings_event_handler(OTA_BREEZE_FLASH_STORE_OK);
                }
                else {
                    p_ota->ota_breeze_status = OTA_BREEZE_STATE_RECEIVE;
                    ota_breeze_send_bytes_received();
                }
            }
            else {
                OTA_LOG_E("ota breeze flash write: flash store failed");
                return;
            }
            break;

        default:
            break;
    }
}

uint32_t ota_breeze_on_fw_upgrade_req(uint8_t *buffer, uint32_t length)
{
    uint32_t num_pages, rx_fw_size;
    uint32_t err_code = 0;
    uint8_t l_len = 0;
    uint8_t send_nack = false;
    uint8_t resume = false;
    _ota_ble_global_dat_t* p_ota = &g_ctx;

    /* Check parameters. */
    if((buffer == NULL) || (length == 0)) {
        return OTA_BREEZE_ERROR_INVALID_PARAM;
    }

    /* Check if resume from previous process. */
    resume = ais_ota_check_if_resume(buffer, length);
    /* Check if the new firmware version is accepted. */
    err_code = ota_breeze_check_upgrade_fw_version(&p_ota->verison, buffer, length);

    if (err_code == OTA_BREEZE_SUCCESS || resume) {
        ais_ota_update_fw_version(buffer, length);

        l_len      = length - sizeof(uint32_t) - sizeof(uint16_t);
        rx_fw_size = EXTRACT_U32(buffer + l_len);
        OTA_LOG_I("rx_fw_size = %d\n", rx_fw_size);

        if (rx_fw_size != 0) {
            p_ota->rx_fw_size   = rx_fw_size;
            p_ota->frames_recvd = 0;
            p_ota->crc = EXTRACT_U16(buffer + l_len + sizeof(uint32_t));
            if (resume) {
                p_ota->bytes_recvd =
                  (p_ota->rx_fw_size == ais_ota_get_setting_fw_offset())
                    ? ais_ota_get_setting_fw_offset()
                    : ais_ota_get_setting_fw_offset() &
                        ~(ais_ota_get_page_size() - 1);
            }
            else {
                p_ota->bytes_recvd = 0;
            }

            /* Check if anything to erase. */
            num_pages = ota_breeze_align_to_page(p_ota->rx_fw_size - p_ota->bytes_recvd,
                                      ais_ota_get_page_size()) / ais_ota_get_page_size();
            if (num_pages == 0) {
                /* Nothing to erase. */
                err_code = ota_breeze_send_fw_upgrade_rsp(true);
                if (err_code == OTA_BREEZE_SUCCESS) {
                    p_ota->ota_breeze_status = OTA_BREEZE_STATE_RECEIVE;
                }
            }
            else {
                /*will erase 1 page first, and erase the reamining pages later, temp workaround.*/
                ota_breeze_g_page_erase_already = p_ota->bytes_recvd / ais_ota_get_page_size();
                ota_breeze_g_page_erase_total = num_pages + ota_breeze_g_page_erase_already;
                err_code = ais_ota_flash_erase((uint32_t const *)(p_ota->bank_1_addr + p_ota->bytes_recvd), 1, /*flash_event_handler*/ NULL);
                ota_breeze_g_page_erase_already++;
                if (err_code != OTA_BREEZE_SUCCESS) {
                    return OTA_BREEZE_ERROR_FLASH_ERASE_FAIL;
                }
                ota_breeze_flash_event_handler(OTA_BREEZE_FLASH_ERASE_OK);
            }
        }
        else {
            send_nack = true;
        }
    }
    else {
        send_nack = true;
    }

    if (send_nack) {
        (void)ota_breeze_send_fw_upgrade_rsp(false);
    }
    return OTA_BREEZE_SUCCESS;
}

/**@brief Function for handling command @ref ALI_CMD_FW_DATA in state
 *        @ref ALI_OTA_STATE_RECEIVE.
 */
void ota_breeze_on_fw_data(uint8_t *buffer, uint32_t length, uint8_t num_frames)
{
    uint32_t err_code;
    uint32_t i = 0;
    uint32_t bin_magic = 0;
    ota_breeze_bin_type_t bin_type;
    static uint16_t last_percent = 0;
    uint16_t        percent;
    _ota_ble_global_dat_t* p_ota = &g_ctx;

    if((buffer == NULL) || (length == 0)) {
        return OTA_BREEZE_ERROR_INVALID_PARAM;
    }

    if ((length & 0x03) != 0) {
        ota_breeze_send_error();
        return;
    }

    if (p_ota->bytes_recvd <= OTA_BREEZE_BIN_TYPE_INFO_OFFSET &&
        p_ota->bytes_recvd + length > OTA_BREEZE_BIN_TYPE_INFO_OFFSET) {
        bin_magic = EXTRACT_U32(buffer + OTA_BREEZE_BIN_TYPE_INFO_OFFSET - p_ota->bytes_recvd);   
        OTA_LOG_I("ota breeze bin file magic detected: 0x%08x.", bin_magic);
        for (i = 0; i < sizeof(ota_breeze_bin_info) / sizeof(ota_breeze_bin_info[0]); i++) {
            if (ota_breeze_bin_info[i].magic == bin_magic) {
                break;
            }
        }

        if (i >= sizeof(ota_breeze_bin_info) / sizeof(ota_breeze_bin_info[0])) {
            OTA_LOG_E("ota breeze bin file magic detected error");
            return;
        }

        bin_type = ota_breeze_bin_info[i].type;

        /* Setting info, on board FW and to-upgrade FW should match (support
         * each other). */
#ifdef AOS_BINS
        if (bin_type == OTA_BREEZE_BIN_TYPE_SINGLE ||
            ais_ota_check_if_bins_supported() == false) {
            OTA_LOG_E("ota breeze don't support single bin");
            return;
        }

        OTA_LOG_I("ota breeze setting OTA bin type 0x%02x ...", bin_type);
        ais_ota_set_upgrade_bin_type_info(bin_type);
#else
        if (bin_type != ALI_OTA_BIN_TYPE_SINGLE ||
            ais_ota_check_if_bins_supported() == true) {
            OTA_LOG_E("ota breeze don't support mults bins");
            return;
        }
#endif
    }

    /* Write Flash. */
    err_code = ais_ota_flash_store(
      (uint32_t const *)(p_ota->bank_1_addr + p_ota->bytes_recvd),
      (uint32_t const *)buffer, length /* >> 2*/, /*flash_event_handler*/ NULL);
    if (err_code != ALI_OTA_FLASH_CODE_SUCCESS) {
        OTA_LOG_E("ota breeze flash erase failed");
        return;
    }

    p_ota->ota_breeze_status = OTA_BREEZE_STATE_WRITE;
    p_ota->bytes_recvd += length;
    p_ota->frames_recvd += num_frames;
    if((p_ota->bytes_recvd % ais_ota_get_page_size()) == 0) {
        if(ota_breeze_g_page_erase_already <= ota_breeze_g_page_erase_total){
            err_code = ais_ota_flash_erase(
            (uint32_t const *)(p_ota->bank_1_addr + (ota_breeze_g_page_erase_already * ais_ota_get_page_size()) ), 1,  NULL);
            if(err_code != ALI_OTA_FLASH_CODE_SUCCESS){
                return;
            }
            else{
                ota_breeze_g_page_erase_already++;
            }
        }
    }
    ota_breeze_flash_event_handler(OTA_BREEZE_FLASH_STORE_OK);

    /* Display progress, 5% as step */
    percent = p_ota->bytes_recvd * 100 / p_ota->rx_fw_size; /* Ensure no overflow */
    if ((percent - last_percent) >= 2) {
        printf("===>%dB\t%d%% ...\r\n", p_ota->bytes_recvd, percent);
        last_percent = percent;
    }
}

uint32_t ota_breeze_get_init_fw_size()
{
    uint32_t err_code;
    _ota_ble_global_dat_t* p_ota = &g_ctx;

    if(ota_breeze_g_page_erase_already <= ota_breeze_g_page_erase_total){
        err_code = ais_ota_flash_erase(
        (uint32_t const *)(p_ota->bank_1_addr + (ota_breeze_g_page_erase_already *ais_ota_get_page_size()) ), 1,  NULL);
        if(err_code != ALI_OTA_FLASH_CODE_SUCCESS){
            return OTA_BREEZE_ERROR_FLASH_ERASE_FAIL;
        }
        else{
            ota_breeze_g_page_erase_already++;
        }
    }
    ota_breeze_send_bytes_received();
    if (p_ota->bytes_recvd >= p_ota->rx_fw_size) {
        p_ota->ota_breeze_status = OTA_BREEZE_STATE_FW_CHECK;
    }
    return OTA_BREEZE_SUCCESS;
}

/**@brief Function for handling command @ref ALI_CMD_FW_XFER_FINISH in state
 *        @ref ALI_OTA_STATE_FW_CHECK.
 */
void ota_breeze_on_xfer_finished(uint8_t *buffer, uint16_t length)
{
    uint32_t err_code;
    uint16_t crc;

    _ota_ble_global_dat_t* p_ota = &g_ctx;

    if((buffer == NULL) || (length == 0)) {
        OTA_LOG_I("breeze ota xfer input paramers error!");
        return;
    }
    OTA_LOG_I("ota breeze:the received fw size: %d", p_ota->rx_fw_size);

    /* Check CRC here. */
    crc = crc16_compute((uint8_t const *)p_ota->bank_1_addr, p_ota->rx_fw_size, NULL);
    OTA_LOG_I("ota breeze:the calculated crc: 0x%x, the read crc: 0x%x", crc, p_ota->crc);
    if (crc == p_ota->crc) {
        int32_t img_crc;

        img_crc = crc32_compute((uint8_t const *)p_ota->bank_1_addr,
                                p_ota->rx_fw_size, NULL);
        ais_ota_update_setting_after_xfer_finished(p_ota->rx_fw_size, img_crc);

        err_code = ais_ota_settings_write(/*bootloader_settings_event_handler*/ NULL);
        if (err_code != ALI_OTA_SETTINGS_CODE_SUCCESS) {
            OTA_LOG_E("ota breeze, ota xfer finished: setting set failed");
            return;
        }

        /* Wait for a while after storing settings and before reboot. */
        ota_msleep(2000);

        p_ota->ota_breeze_status = OTA_BREEZE_STATE_RESET_PREPARE;
        ota_breeze_bootloader_settings_event_handler(OTA_BREEZE_FLASH_STORE_OK);
    }
    else {
        err_code = ota_breeze_send_crc_result(false);
        if (err_code == OTA_BREEZE_SUCCESS) {
            p_ota->ota_breeze_status = OTA_BREEZE_STATE_IDLE;
        }
        ais_ota_set_setting_fw_offset(0);
        err_code = ais_ota_settings_write(NULL);
        if(err_code != OTA_BREEZE_SUCCESS) {
            OTA_LOG_E("ota breeze clear the settings rec length failed");
        }
    }
}

void ota_breeze_reset()
{
    _ota_ble_global_dat_t* p_ota = &g_ctx;
    
    OTA_LOG_I("ALI_EVT_DISCONNECTED\r\n");
    /* Reset state machine. */
    p_ota->ota_breeze_status = OTA_BREEZE_STATE_OFF;
    p_ota->rx_fw_size   = 0;
    p_ota->bytes_recvd  = 0;
    p_ota->frames_recvd = 0;
    p_ota->crc          = 0;

    if (ota_breeze_new_fw) {
        OTA_LOG_I("Firmware download completed, system will reboot now!");
        ota_reboot();
    }
}


static void ota_breeze_on_tx_done(uint8_t cmd)
{
    _ota_ble_global_dat_t* p_ota = &g_ctx;

    /* Check if it is the correct state. */
    switch (p_ota->ota_breeze_status) {
        case OTA_BREEZE_STATE_RESET_PREPARE:
            OTA_LOG_I("breeeze ota reset prepare!!!");
            if (cmd == OTA_BREEZE_CMD_FW_CHECK_RESULT) {
                ota_breeze_new_fw = true;
                ota_breeze_disconnect();
                OTA_LOG_I("OK, now we must disconnect ble!");
            }
            break;

        case OTA_BREEZE_STATE_RECEIVE_ERR:
            if (cmd == OTA_BREEZE_CMD_ERROR) {
                ota_breeze_send_bytes_received();
            }
            else if (cmd == OTA_BREEZE_CMD_FW_BYTES_RECEIVED) {
                p_ota->ota_breeze_status = OTA_BREEZE_STATE_RECEIVE;
            }
            break;

        default:
            break;
    }
}

static void ota_breeze_on_auth(uint8_t is_authenticated)
{
    uint32_t err_code;
    _ota_ble_global_dat_t* p_ota = &g_ctx;
    
    if (!is_authenticated) {
        return;
    }

    if (p_ota->ota_breeze_status == OTA_BREEZE_STATE_OFF) {
        /* Check if image swapping has happened before boot. */
        if (ais_ota_check_if_update_finished()) {
            OTA_LOG_I("Image swapping performed before, let's notify fw upgrade done.");
            ais_ota_update_settings_after_update_finished();
            err_code = ais_ota_settings_write(NULL);
            if (err_code != ALI_OTA_SETTINGS_CODE_SUCCESS) {
                OTA_LOG_E("ota breeze on auth: new fw write setting failed");
                return;
            }
            p_ota->ota_breeze_status = OTA_BREEZE_STATE_UPGRADE_REPORT;
            ota_breeze_bootloader_settings_event_handler(OTA_BREEZE_FLASH_STORE_OK);
        }
        else if (p_ota->feature_enable) {
            OTA_LOG_I("ota breeze on auth:status change into idle model.");
            p_ota->ota_breeze_status = OTA_BREEZE_STATE_IDLE;
        }
    }
    else {
        OTA_LOG_E("ota breeze on auth:error status.");
    }
}

static void ota_breeze_on_discontinuous_frame()
{
    _ota_ble_global_dat_t* p_ota = &g_ctx;
    
    /* Reactive only when receiving FW data. */
    if (p_ota->ota_breeze_status == OTA_BREEZE_STATE_RECEIVE) {
        p_ota->ota_breeze_status = OTA_BREEZE_STATE_RECEIVE_ERR;
    }
    ota_reboot();
}


void ota_breeze_event_dispatcher(uint8_t event_type, uint8_t sub_status)
{
    OTA_LOG_I("event:%d, sub_event:%d", event_type, sub_status);
    switch(event_type) {
        case ALI_OTA_ON_AUTH_EVT:
            ota_breeze_on_auth(sub_status);
            break;
        case ALI_OTA_ON_TX_DONE:
            ota_breeze_on_tx_done(sub_status);
            break;
        case ALI_OTA_ON_DISCONNECTED:
            ota_msleep(1000);
            g_ctx.ota_breeze_task_active_ctrl = false;
            ota_msleep(1000);
            ota_breeze_reset();
            break;
        case ALI_OTA_ON_DISCONTINUE_ERR:
            ota_msleep(1000);
            g_ctx.ota_breeze_task_active_ctrl = false;
            ota_msleep(1000);
            ota_breeze_on_discontinuous_frame();
            break;
        default:
            OTA_LOG_I("ota breeze get a unknow event");
            break;
    }
}

void ota_breeze_cmd_disptacher(void)
{
    uint32_t err_code = 0;
    bool send_err = false;
    ota_breeze_version_t* tmp_verion = NULL;
    ota_breeze_rec_t tmp_queue;
    ota_breeze_state_t cur_breeze_status;
    if(ota_breeze_receive_data_consume(&tmp_queue) == OTA_BREEZE_SUCCESS) {
        if ((tmp_queue.cmd & OTA_BREEZE_CMD_TYPE_MASK) != OTA_BREEZE_CMD_TYPE_FW_UPGRADE) {
               OTA_LOG_E("ota breeze receive error cmd");
        }
        else {
	    cur_breeze_status = g_ctx.ota_breeze_status;
            switch(cur_breeze_status) {
                case OTA_BREEZE_STATE_IDLE:
                    if (tmp_queue.cmd == OTA_BREEZE_CMD_FW_VERSION_REQ) { // cmd=0x20
                        tmp_verion = &g_ctx.verison;
                        err_code = ota_breeze_send_fw_version_rsp(OTA_BREEZE_CMD_FW_VERSION_RSP, tmp_verion->fw_ver, tmp_verion->fw_ver_len);
                        if(err_code != OTA_BREEZE_SUCCESS) {
                            ota_breeze_err_hdl(err_code);
                        }
                    }
                    else if (tmp_queue.cmd == OTA_BREEZE_CMD_FW_UPGRADE_REQ) { // cmd=0x22
                        err_code = ota_breeze_on_fw_upgrade_req(tmp_queue.rec_buf, tmp_queue.length);
                        if(err_code != OTA_BREEZE_SUCCESS) {
                            ota_breeze_err_hdl(err_code);
                        }
                    }
                    else {
                        send_err = true;
                    }
                    break;
 
                case OTA_BREEZE_STATE_RECEIVE:
                    if (tmp_queue.cmd == OTA_BREEZE_CMD_FW_DATA) { // cmd=0x2F
                        ota_breeze_on_fw_data(tmp_queue.rec_buf, tmp_queue.length, tmp_queue.num_frames);
                    }
                    else if (tmp_queue.cmd == OTA_BREEZE_CMD_FW_GET_INIT_FW_SIZE) { // cmd=0x27
                        if(ota_breeze_get_init_fw_size() != OTA_BREEZE_SUCCESS) {
                          send_err = true;
                        }
                    }
                    else {
                        send_err = true;
                    }
                    break;
                case OTA_BREEZE_STATE_FW_CHECK:
                    if (tmp_queue.cmd == OTA_BREEZE_CMD_FW_XFER_FINISH) { // cmd=0x28
                        ota_breeze_on_xfer_finished(tmp_queue.rec_buf, tmp_queue.length);
                        OTA_LOG_I("Firmware download completed, let's set the flag.\r\n");
                    }
                    else {
                        send_err = true;
                    }
                    break;
                case OTA_BREEZE_STATE_UPGRADE_REPORT:
                case OTA_BREEZE_STATE_RESET_PREPARE:
                case OTA_BREEZE_STATE_OFF:
                case OTA_BREEZE_STATE_WRITE:
                    send_err = true;
                    break;
                default:
                    break;
                }
            }
            if (send_err) {
                (void)ota_breeze_send_error();
            }
    }
}
