from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_main_firmware_declares_robot_uart_bridge():
    source = read("main/robot_uart.cc")
    header = read("main/robot_uart.h")

    assert "class RobotUart" in header
    assert "ROBOT_UART_TX_PIN" in source
    assert "ROBOT_UART_RX_PIN" in source
    assert "uart_write_bytes" in source
    assert "WaitForAck" in source
    assert "No UART ACK from servant" in source
    assert '\\"cmd\\":\\"servo\\"' in source
    assert '\\"part\\":\\"' in source
    assert 'left_arm' in source
    assert '\\"action\\":\\"raise\\"' in source


def test_application_triggers_left_arm_from_websocket_and_mcp():
    app_cc = read("main/application.cc")
    app_h = read("main/application.h")
    mcp_cc = read("main/mcp_server.cc")

    assert '#include "robot_uart.h"' in app_cc
    assert "RobotUart robot_uart_" in app_h
    assert "HandleRobotActionMessage" in app_cc
    assert '"robot_action"' in app_cc
    assert '"left_arm_raise"' in app_cc
    assert "self.robot.left_arm_raise" in mcp_cc
    assert "SendLeftArmRaise" in mcp_cc


def test_freenove_board_exposes_uart_pins_not_used_by_lcd_audio():
    config = read("main/boards/freenove-esp32s3-display-2.8-lcd/config.h")

    assert "ROBOT_UART_NUM" in config
    assert "ROBOT_UART_TX_PIN      GPIO_NUM_17" in config
    assert "ROBOT_UART_RX_PIN      GPIO_NUM_18" in config
    assert "ROBOT_UART_ALT_TX_PIN  GPIO_NUM_43" in config
    assert "ROBOT_UART_ALT_RX_PIN  GPIO_NUM_44" in config
    assert "ROBOT_UART_BAUD_RATE   115200" in config


def test_servant_firmware_has_uart_servo_controller():
    main_c = read("../TBOT-Servant-Firmware/main/main.c")

    assert "SERVO_GPIO GPIO_NUM_13" in main_c
    assert "UART_PORT UART_NUM_1" in main_c
    assert "LEDC_TIMER_50HZ" in main_c
    assert "servo_sweep" in main_c
    assert "cJSON_GetObjectItem" in main_c
    assert '"left_arm"' in main_c
    assert '"raise"' in main_c
