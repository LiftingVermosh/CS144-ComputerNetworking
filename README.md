# README

## 与上游仓库的关系

原 24 win asst 的 offical 貌似已挂，本仓库 fork 自 kangyupl 大佬的 [gitee 备份](https://gitee.com/kangyupl/cs144.github.io)，感谢 kangyupl 大佬开源的原仓库，以下为新的说明

## 文档

- [writeups/](writeups/)：按课程要求提交的实验报告（`labN.md` 英文、`labN_zh.md` 中文），含程序结构与设计、实现难点、遗留缺陷；

## 构建与测试

实验本机 `PATH` 里使用的是 conda 的 `g++ 10.4`，而讲义要求 C++23（Ubuntu 24.04 LTS 上的 `g++ 13.3`），因此配置时必须显式指定编译器：

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=/usr/bin/g++-13
cmake --build build -j
cd build && ctest -R 'byte_stream|strm_reassem' --output-on-failure
```

> 其他想到了再填