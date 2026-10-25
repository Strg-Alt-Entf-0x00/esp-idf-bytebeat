# UART Filebridge

The `console_bytebeat_dev` example declares the external
`esp-uart-filebridge` component in
`examples/console_bytebeat_dev/main/idf_component.yml`. The bridge provides a
separate host-to-device file-transfer path; it is not the Bytebeat formula
serial protocol.

The example README documents UART1 at 3,000,000 baud and the host commands
below. The COM port is host-specific; use the board's actual port.

```powershell
python -m esp_idf_uart_filebridge.cli --port COMx --baud 3000000 ls /sd
python -m esp_idf_uart_filebridge.cli --port COMx --baud 3000000 ls /sd/telemetry
```

The CLI must be installed and available in the active Python environment.
Consult the external Filebridge documentation for installation and supported
commands.

The example README describes SD-card telemetry capture, but the telemetry
component and capture pipeline it outlines are marked as planned in that same
README and are not present in the inspected example source. Treat the paths
and listing commands as documented intentions until verified on the target.
A working Filebridge connection alone does not establish that telemetry files
exist. Verify the files and firmware behavior on the target before treating
telemetry capture as available.

Do not run another program on the Filebridge serial port during a transfer.
The bridge's UART and baud rate are distinct from the firmware console port;
check the active board configuration before connecting.
