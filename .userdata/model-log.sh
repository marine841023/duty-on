#!/bin/bash
# 设备端：最近的模型相关日志
journalctl -u dutyon.service --no-pager | grep -a "Model\|model loaded" | tail -6
