# NVIDIA GPU 开源内核模块项目概述

## 项目目标
- 维护和开发NVIDIA GPU的开源Linux内核模块
- 提供GPU驱动程序的关键功能实现
- 确保与Linux内核的兼容性和稳定性
- 支持NVIDIA显卡的核心功能和特性

## 项目范围
- GPU驱动核心功能(kernel-open/nvidia/)
- DRM接口实现(kernel-open/nvidia-drm/)
- 模式设置支持(kernel-open/nvidia-modeset/)
- 内存管理与UVM支持(kernel-open/nvidia-uvm/)
- P2P内存访问支持(kernel-open/nvidia-peermem/)

## 核心组件
- 主驱动模块(nvidia.ko)
- DRM模块(nvidia-drm.ko)
- 模式设置模块(nvidia-modeset.ko)
- UVM模块(nvidia-uvm.ko)
- P2P内存模块(nvidia-peermem.ko)

## 技术栈
- 编程语言: C
- 构建系统: Kbuild/Make
- 目标平台: Linux内核
- 关键依赖: Linux内核API
