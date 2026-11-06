/**
 * @file hangar_bridge.c
 * @brief 机库通信桥实现。
 *
 * 数据流：裁判模拟器 --(USART1, R27 official frame)--> 机库 --(USART6,
 * Bluetooth transparent serial, same R27 frame)--> LicheeRV nano。
 * USART1 上的状态回传使用官方 0x0301 机器人交互帧，子内容 0x02F0 仅为
 * 本工程约定的诊断数据，不改变 0x0001/0x020A 的裁判系统语义。
 */
#include "hangar_bridge.h"

#include "bsp_log.h"
#include "bsp_usart.h"
#include "string.h"
#include "usart.h"

#define HANGAR_STATUS_DATA_LENGTH 112u
#define HANGAR_INTERACTION_LENGTH 118u
#define HANGAR_STATUS_FRAME_LENGTH (R27_LEN_HEADER + R27_LEN_CMDID + HANGAR_INTERACTION_LENGTH + R27_LEN_TAIL)

static R27Link *s_referee_link;
static USARTInstance *s_aircraft_usart;
static HangarStatusPayload s_status;
static R27ReceivedFrame s_pending_frame;
static uint8_t s_pending_valid;
static uint32_t s_last_rx_error_count;
static uint32_t s_uptime_ms;
static uint8_t s_status_frame[HANGAR_STATUS_FRAME_LENGTH];

static void HangarStatusReset(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.magic[0] = HANGAR_STATUS_MAGIC_0;
    s_status.magic[1] = HANGAR_STATUS_MAGIC_1;
    s_status.magic[2] = HANGAR_STATUS_MAGIC_2;
    s_status.magic[3] = HANGAR_STATUS_MAGIC_3;
    s_status.version = 1;
    s_status.status = HANGAR_STATUS_IDLE;
}

static void HangarStatusRefreshCounters(void)
{
    if (s_referee_link == NULL)
        return;

    s_status.parsed_frame_count = s_referee_link->info.rx_frame_cnt;
    s_status.rx_error_count = s_referee_link->info.rx_error_cnt;
    s_status.uptime_ms = s_uptime_ms;
}

static void HangarStatusSend(void)
{
    uint8_t interaction[HANGAR_INTERACTION_LENGTH] = {0};
    uint16_t frame_length;

    if (s_referee_link == NULL)
        return;

    /* 0x0301 data = sub-content ID + sender + receiver + 112-byte content. */
    interaction[0] = (uint8_t)(HANGAR_STATUS_DATA_CMD_ID & 0xFFu);
    interaction[1] = (uint8_t)(HANGAR_STATUS_DATA_CMD_ID >> 8);
    /* sender/receiver are local diagnostic endpoints, not a competition robot ID. */
    interaction[2] = 0;
    interaction[3] = 0;
    interaction[4] = 0x80;
    interaction[5] = 0x80;
    memcpy(interaction + R27_INTERACTION_HEAD_LEN, &s_status, sizeof(s_status));

    frame_length = R27BuildFrame(R27_ID_robot_interaction, interaction,
                                 HANGAR_INTERACTION_LENGTH, R27RefereeNextSeq(),
                                 s_status_frame, sizeof(s_status_frame));
    if (frame_length != HANGAR_STATUS_FRAME_LENGTH)
    {
        LOGERROR("[hangar_bridge] build status frame failed");
        return;
    }

    /* RobotTask runs at 5 ms; never block it waiting for a previous diagnostic
     * DMA transfer. A later command will produce another status frame. */
    if (s_referee_link->usart == NULL ||
        s_referee_link->usart->usart_handle->gState != HAL_UART_STATE_READY)
        return;

    USARTSend(s_referee_link->usart, s_status_frame, frame_length, USART_TRANSFER_DMA);
}

static uint8_t HangarIsOwnStatus(const R27ReceivedFrame *frame)
{
    if (frame == NULL || frame->cmd_id != R27_ID_robot_interaction || frame->data_length < R27_INTERACTION_HEAD_LEN + 4u)
        return 0;

    return (frame->frame[R27_DATA_OFFSET] == (uint8_t)(HANGAR_STATUS_DATA_CMD_ID & 0xFFu) &&
            frame->frame[R27_DATA_OFFSET + 1u] == (uint8_t)(HANGAR_STATUS_DATA_CMD_ID >> 8) &&
            frame->frame[R27_DATA_OFFSET + R27_INTERACTION_HEAD_LEN] == HANGAR_STATUS_MAGIC_0 &&
            frame->frame[R27_DATA_OFFSET + R27_INTERACTION_HEAD_LEN + 1u] == HANGAR_STATUS_MAGIC_1 &&
            frame->frame[R27_DATA_OFFSET + R27_INTERACTION_HEAD_LEN + 2u] == HANGAR_STATUS_MAGIC_2 &&
            frame->frame[R27_DATA_OFFSET + R27_INTERACTION_HEAD_LEN + 3u] == HANGAR_STATUS_MAGIC_3);
}

static void HangarPrepareStatus(const R27ReceivedFrame *frame)
{
    r27_referee_info_t *info = &s_referee_link->info;

    s_status.last_cmd_id = frame->cmd_id;
    s_status.last_seq = frame->seq;
    s_status.last_data_length = frame->data_length;
    s_status.game_progress = info->GameState.game_progress;
    s_status.dart_launch_opening_status = info->DartClientCmd.dart_launch_opening_status;
    HangarStatusRefreshCounters();
}

uint8_t HangarBridgeInit(UART_HandleTypeDef *referee_handle, UART_HandleTypeDef *aircraft_handle)
{
    USART_Init_Config_s aircraft_config;

    if (referee_handle == NULL || aircraft_handle == NULL)
        return 0;

    HangarStatusReset();
    s_uptime_ms = 0;
    s_pending_valid = 0;
    s_last_rx_error_count = 0;

    s_referee_link = R27RefereeInit(R27_LINK_COMMON, referee_handle);
    if (s_referee_link == NULL)
    {
        LOGERROR("[hangar_bridge] USART1 referee link init failed");
        return 0;
    }

    memset(&aircraft_config, 0, sizeof(aircraft_config));
    aircraft_config.module_callback = NULL; /* LicheeRV link is transparent TX/RX. */
    aircraft_config.usart_handle = aircraft_handle;
    aircraft_config.recv_buff_size = 1;
    s_aircraft_usart = USARTRegister(&aircraft_config);
    if (s_aircraft_usart == NULL)
    {
        LOGERROR("[hangar_bridge] USART6 Bluetooth link init failed");
        return 0;
    }

    LOGINFO("[hangar_bridge] referee USART1 -> Bluetooth USART6 ready");
    return 1;
}

static void HangarForwardPending(void)
{
    if (!s_pending_valid || s_aircraft_usart == NULL || s_referee_link == NULL)
        return;

    if (s_aircraft_usart->usart_handle->gState != HAL_UART_STATE_READY)
        return; /* Keep the frame pending until the previous DMA transfer completes. */

    USARTSend(s_aircraft_usart, s_pending_frame.frame, s_pending_frame.length, USART_TRANSFER_DMA);
    s_status.forwarded_frame_count++;
    s_status.forward_error_count = 0;
    s_status.status = HANGAR_STATUS_FORWARDED;
    HangarPrepareStatus(&s_pending_frame);
    s_pending_valid = 0;
    HangarStatusSend();
}

void HangarBridgeTask(void)
{
    r27_referee_info_t *info;

    if (s_referee_link == NULL || s_aircraft_usart == NULL)
        return;

    s_uptime_ms += 5;
    info = &s_referee_link->info;
    HangarStatusRefreshCounters();

    /* A bad CRC has no complete command to forward, but the simulator still gets
     * a diagnostic status so a cable/noise problem is distinguishable from silence. */
    if (info->rx_error_cnt != s_last_rx_error_count)
    {
        s_last_rx_error_count = info->rx_error_cnt;
        s_status.status = HANGAR_STATUS_RX_ERROR;
        s_status.last_cmd_id = 0;
        s_status.last_data_length = 0;
        HangarStatusSend();
    }

    if (!s_pending_valid)
        s_pending_valid = R27RefereePopFrame(s_referee_link, &s_pending_frame);

    if (s_pending_valid && HangarIsOwnStatus(&s_pending_frame))
    {
        /* Ignore a USB adapter that echoes our diagnostic response. */
        s_pending_valid = 0;
    }

    HangarForwardPending();
}

const HangarStatusPayload *HangarBridgeGetStatus(void)
{
    return &s_status;
}
