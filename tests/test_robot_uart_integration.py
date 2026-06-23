from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_main_firmware_declares_robot_uart_bridge():
    source = read("main/robot_uart.cc")
    header = read("main/robot_uart.h")

    assert "class RobotUart" in header
    for method in [
        "SendLeftArmRaise",
        "SendRightArmRaise",
        "SendLeftArmLower",
        "SendRightArmLower",
        "SendBothArmsRaise",
        "SendBothArmsLower",
        "SendLeftArmSetPercent",
        "SendRightArmSetPercent",
        "SendBothArmsSetPercent",
        "SendHeadTurnLeft",
        "SendHeadTurnRight",
        "SendHeadCenter",
        "SendHeadSetAngle",
        "SendHeadSetPercent",
    ]:
        assert method in header
        assert method in source
    assert "ROBOT_UART_TX_PIN" in source
    assert "ROBOT_UART_RX_PIN" in source
    assert "uart_write_bytes" in source
    assert "WaitForAck" in source
    assert "No UART ACK from servant" in source
    assert "Retrying robot command on alternate UART pins" in source
    assert "SendPayloadOnProfile" in header
    assert '\\"cmd\\":\\"servo\\"' in source
    assert '\\"part\\":\\"' in source
    assert 'left_arm' in source
    assert 'right_arm' in source
    assert 'head' in source
    assert '\\"action\\":\\"' in source
    assert 'action == "raise"' in source
    assert 'turn_left' in source
    assert 'turn_right' in source
    assert 'center' in source
    assert 'set_angle' in source
    assert 'set_percent' in source
    assert 'clamp_servo_angle' in source
    assert 'clamp_percent' in source
    assert 'lower' in source


def test_application_triggers_left_arm_from_websocket_and_mcp():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")
    mcp_cc = read("main/mcp_server.cc")

    assert '#include "robot_uart.h"' in app_cc
    assert "RobotUart robot_uart_" in app_h
    assert "HandleRobotActionMessage" in app_cc
    assert "HandleEmotionGesture" in app_cc
    assert '"robot_action"' in app_cc
    for action in [
        "left_arm_raise",
        "right_arm_raise",
        "left_arm_lower",
        "right_arm_lower",
        "both_arms_raise",
        "both_arms_lower",
        "left_arm_set_percent",
        "right_arm_set_percent",
        "both_arms_set_percent",
        "head_turn_left",
        "head_turn_right",
        "head_center",
        "head_set_angle",
        "head_set_percent",
    ]:
        assert f'"{action}"' in app_cc
        assert f"self.robot.{action}" in mcp_cc
    assert "SendBothArmsRaise" in app_cc
    assert "SendLeftArmRaise" in app_cc
    assert "SendLeftArmSetPercent" in app_cc
    assert "SendRightArmSetPercent" in app_cc
    assert "SendBothArmsSetPercent" in app_cc
    assert "SendHeadTurnLeft" in app_cc
    assert "SendHeadSetAngle" in app_cc
    assert "SendHeadSetPercent" in app_cc
    assert "quay đầu trái" in mcp_cc
    assert "quay đầu phải" in mcp_cc
    assert "đưa đầu về giữa" in mcp_cc
    assert "self.robot.head_set_angle" in mcp_cc
    assert 'Property("angle", kPropertyTypeInteger, 90, 0, 180)' in mcp_cc
    assert "chỉnh góc quay đầu" in mcp_cc
    assert "self.robot.head_set_percent" in mcp_cc
    assert 'Property("percent", kPropertyTypeInteger, 50, 0, 100)' in mcp_cc
    assert "quay đầu 50%" in mcp_cc
    assert "self.robot.left_arm_set_percent" in mcp_cc
    assert "self.robot.right_arm_set_percent" in mcp_cc
    assert "self.robot.both_arms_set_percent" in mcp_cc
    assert 'Property("percent", kPropertyTypeInteger, 100, 0, 100)' in mcp_cc
    assert "nâng tay trái 50%" in mcp_cc
    assert "dơ tay trái" in mcp_cc
    assert "dơ tay phải" in mcp_cc
    assert "dơ cả hai tay" in mcp_cc

def test_llm_emotion_messages_do_not_trigger_arm_gestures():
    app_cc = read("main/application.cc")
    match = re.search(
        r"void Application::HandleEmotionGesture\(const char\* emotion\) \{(?P<body>.*?)\n\}",
        app_cc,
        re.S,
    )
    assert match, "HandleEmotionGesture function missing"

    body = match.group("body")
    assert "SendLeftArmRaise" not in body
    assert "SendRightArmRaise" not in body
    assert "SendBothArmsRaise" not in body
    assert "SendLeftArmLower" not in body
    assert "SendRightArmLower" not in body
    assert "SendBothArmsLower" not in body
    assert "SendLeftArmSetPercent" not in body
    assert "SendRightArmSetPercent" not in body
    assert "SendBothArmsSetPercent" not in body
    assert "SendHeadTurnLeft" not in body
    assert "SendHeadTurnRight" not in body
    assert "SendHeadCenter" not in body
    assert "SendHeadSetAngle" not in body
    assert "SendHeadSetPercent" not in body


def test_right_arm_uses_mirrored_servo_sweep_direction():
    source = read("main/robot_uart.cc")

    assert 'part == "right_arm" && action == "raise"' in source
    assert 'return SendServoSweep(part, action, 60, 0, 2, 20);' in source
    assert 'part == "right_arm" && action == "lower"' in source
    assert 'return SendServoSweep(part, action, 0, 60, 2, 20);' in source


def test_freenove_board_exposes_uart_pins_not_used_by_lcd_audio():
    config = read("main/boards/freenove-esp32s3-display-2.8-lcd/config.h")

    assert "ROBOT_UART_NUM" in config
    assert "ROBOT_UART_TX_PIN      GPIO_NUM_17" in config
    assert "ROBOT_UART_RX_PIN      GPIO_NUM_18" in config
    assert "ROBOT_UART_ALT_TX_PIN  GPIO_NUM_43" in config
    assert "ROBOT_UART_ALT_RX_PIN  GPIO_NUM_44" in config
    assert "ROBOT_UART_BAUD_RATE   115200" in config

def test_lcdwiki_board_retries_swapped_uart_pins_for_servant_ack():
    config = read("main/boards/lcdwiki-es3c35p/config.h")

    assert "ROBOT_UART_NUM        UART_NUM_1" in config
    assert "ROBOT_UART_TX_PIN     GPIO_NUM_44" in config
    assert "ROBOT_UART_RX_PIN     GPIO_NUM_43" in config
    assert "ROBOT_UART_ALT_NUM    UART_NUM_1" in config
    assert "ROBOT_UART_ALT_TX_PIN GPIO_NUM_43" in config
    assert "ROBOT_UART_ALT_RX_PIN GPIO_NUM_44" in config


def test_servant_firmware_has_uart_servo_controller():
    main_c = read("../TBOT-Servant-Firmware/main/main.c")

    assert "LEFT_ARM_SERVO_GPIO GPIO_NUM_12" in main_c
    assert "RIGHT_ARM_SERVO_GPIO GPIO_NUM_13" in main_c
    assert "HEAD_SERVO_GPIO GPIO_NUM_11" in main_c
    assert "LEFT_ARM_SERVO_LEDC_CHANNEL LEDC_CHANNEL_0" in main_c
    assert "RIGHT_ARM_SERVO_LEDC_CHANNEL LEDC_CHANNEL_1" in main_c
    assert "HEAD_SERVO_LEDC_CHANNEL LEDC_CHANNEL_2" in main_c
    assert "UART_PORT UART_NUM_0" in main_c
    assert "ESP_ERR_INVALID_STATE" in main_c
    assert "LEDC_TIMER_50HZ" in main_c
    assert "servo_sweep" in main_c
    assert "find_servo" in main_c
    assert "cJSON_GetObjectItem" in main_c
    assert '"left_arm"' in main_c
    assert '"right_arm"' in main_c
    assert '"head"' in main_c
    assert '"raise"' in main_c
    assert '"lower"' in main_c
    assert '"turn_left"' in main_c
    assert '"turn_right"' in main_c
    assert '"center"' in main_c
    assert '"set_angle"' in main_c
    assert '"set_percent"' in main_c

def test_head_set_angle_uses_adjustable_servo_angle_payload():
    source = read("main/robot_uart.cc")
    header = read("main/robot_uart.h")
    app_cc = read("main/application.cc")

    assert "bool SendHeadSetAngle(int angle)" in header
    assert "bool SendHeadSetPercent(int percent)" in header
    assert "bool RobotUart::SendHeadSetAngle(int angle)" in source
    assert 'return SendServoSweep("head", "set_angle", 90, target_angle, 2, 20);' in source
    assert "clamp_servo_angle(angle)" in source
    assert 'strcmp(action->valuestring, "head_set_angle") == 0' in app_cc
    assert 'cJSON_GetObjectItem(root, "angle")' in app_cc

def test_percent_commands_map_to_servo_payloads():
    source = read("main/robot_uart.cc")
    header = read("main/robot_uart.h")
    app_cc = read("main/application.cc")

    for method in [
        "bool SendLeftArmSetPercent(int percent)",
        "bool SendRightArmSetPercent(int percent)",
        "bool SendBothArmsSetPercent(int percent)",
        "bool SendHeadSetPercent(int percent)",
    ]:
        assert method in header

    assert "percent_to_left_arm_angle" in source
    assert "percent_to_right_arm_angle" in source
    assert "percent_to_head_angle" in source
    assert 'SendServoSweep("left_arm", "set_percent", 0, target_angle, 2, 20)' in source
    assert 'SendServoSweep("right_arm", "set_percent", 60, target_angle, 2, 20)' in source
    assert 'SendServoSweep("head", "set_percent", 90, target_angle, 2, 20)' in source
    assert 'strcmp(action->valuestring, "left_arm_set_percent") == 0' in app_cc
    assert 'strcmp(action->valuestring, "right_arm_set_percent") == 0' in app_cc
    assert 'strcmp(action->valuestring, "both_arms_set_percent") == 0' in app_cc
    assert 'strcmp(action->valuestring, "head_set_percent") == 0' in app_cc
    assert 'cJSON_GetObjectItem(root, "percent")' in app_cc
