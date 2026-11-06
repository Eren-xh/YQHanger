/**
 * @file hangar_bridge.h
 * @brief 机库通信桥：USART1 接收 2027 裁判帧，USART6 透明转发给 LicheeRV nano。
 */
#ifndef HANGAR_BRIDGE_H
#define HANGAR_BRIDGE_H

#include "stdint.h"
#include "main.h"
#include "rm_referee27.h"

/* 0x0301 是协议规定的机器人交互数据，0x02F0 是本工程保留的机库状态子内容 ID。
 * 该子内容只用于电脑模拟器查看接收/解析/转发结果，不发送给无人机上位机。 */
#define HANGAR_STATUS_DATA_CMD_ID 0x02F0u
#define HANGAR_STATUS_MAGIC_0 'H'
#define HANGAR_STATUS_MAGIC_1 '2'
#define HANGAR_STATUS_MAGIC_2 '7'
#define HANGAR_STATUS_MAGIC_3 'S'

typedef enum
{
    HANGAR_STATUS_IDLE = 0,
    HANGAR_STATUS_FORWARDED = 1,
    HANGAR_STATUS_RX_ERROR = 2,
    HANGAR_STATUS_FORWARD_PENDING = 3,
} HangarStatusCode_e;

#pragma pack(push, 1)
/* 0x02F0 的 112 字节内容，前 34 字节有效，其余为保留位。 */
typedef struct
{
    uint8_t magic[4];
    uint8_t version;
    uint8_t status;
    uint16_t last_cmd_id;
    uint8_t last_seq;
    uint8_t game_progress;
    uint8_t dart_launch_opening_status;
    uint8_t reserved_0;
    uint16_t last_data_length;
    uint32_t parsed_frame_count;
    uint32_t forwarded_frame_count;
    uint32_t rx_error_count;
    uint32_t forward_error_count;
    uint32_t uptime_ms;
    uint8_t reserved[78];
} HangarStatusPayload;
#pragma pack(pop)

_Static_assert(sizeof(HangarStatusPayload) == 112, "hangar status payload must be 112 bytes");

/** @brief 初始化 USART1 裁判链路和 USART6 蓝牙透明转发链路。 */
uint8_t HangarBridgeInit(UART_HandleTypeDef *referee_handle, UART_HandleTypeDef *aircraft_handle);

/** @brief 在应用任务中调用，建议周期 5 ms。 */
void HangarBridgeTask(void);

/** @brief 获取当前桥接状态，供上层调试或单元测试使用。 */
const HangarStatusPayload *HangarBridgeGetStatus(void);

#endif
