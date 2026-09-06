#!/bin/bash
# systemd 自启：dutyon-pet 设备端
set -e
cat > /etc/systemd/system/dutyon.service <<'EOF'
[Unit]
Description=DutyOn Pet Display (Live2D on framebuffer)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/dutyon
ExecStart=/opt/dutyon/dutyon-pet
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable dutyon.service
systemctl start dutyon.service
sleep 8
systemctl is-active dutyon.service
journalctl -u dutyon.service -n 25 --no-pager
