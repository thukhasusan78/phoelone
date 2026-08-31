# MCP IoT Control Usage

This file is the English translation of the former Chinese `docs/mcp-usage_zh.md`.

**Complete specification:** [mcp-usage.md](./mcp-usage.md)

MCP is the only supported IoT control path. The legacy `type: "iot"` protocol is deprecated.

Register tools with `McpServer::AddTool` (visible to the LLM) or `McpServer::AddUserOnlyTool` (companion-app only; listed when `withUserTools=true`).

Built-in AI-callable tools: `self.get_device_status`, `self.audio_speaker.set_volume`, `self.screen.set_brightness`, `self.screen.set_theme`, `self.camera.take_photo` (if a camera exists).

Built-in user-only tools: `self.get_system_info`, `self.reboot`, `self.upgrade_firmware`, `self.screen.get_info`, `self.screen.snapshot`, `self.screen.preview_image`, `self.assets.set_download_url`.

Otto / Mickey additionally registers `self.otto.*`, `self.battery.get_level`, and `self.mickey.*` / `self.phoe_lone.*` sensors (IMU and touch are wired on the mickey no-camera SKU; light is still a stub). Full schemas: [backend_spec.md](../backend_spec.md).
