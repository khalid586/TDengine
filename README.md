<p>
<p align="center">
  <a href="https://tdengine.com" target="_blank">
  <img
    src="docs/assets/tdengine.svg"
    alt="TDengine"
    width="500"
  />
  </a>
</p>
<p>

[![Build Status](https://cloud.drone.io/api/badges/taosdata/TDengine/status.svg?ref=refs/heads/master)](https://cloud.drone.io/taosdata/TDengine)
[![Build status](https://ci.appveyor.com/api/projects/status/kf3pwh2or5afsgl9/branch/master?svg=true)](https://ci.appveyor.com/project/sangshuduo/tdengine-2n8ge/branch/master)
[![Coverage Status](https://coveralls.io/repos/github/taosdata/TDengine/badge.svg?branch=develop)](https://coveralls.io/github/taosdata/TDengine?branch=develop)
[![CII Best Practices](https://bestpractices.coreinfrastructure.org/projects/4201/badge)](https://bestpractices.coreinfrastructure.org/projects/4201)

English | [简体中文](README-CN.md) | [TDengine Cloud](https://cloud.tdengine.com) | [Learn more about TSDB](https://tdengine.com/tsdb/)

# What is TDengine？

TDengine is an open source, high-performance, cloud native [time-series database](https://tdengine.com/tsdb/) optimized for Internet of Things (IoT), Connected Cars, and Industrial IoT. It enables efficient, real-time data ingestion, processing, and monitoring of TB and even PB scale data per day, generated by billions of sensors and data collectors. TDengine differentiates itself from other time-series databases with the following advantages:

- **[High Performance](https://tdengine.com/tdengine/high-performance-time-series-database/)**: TDengine is the only time-series database to solve the high cardinality issue to support billions of data collection points while out performing other time-series databases for data ingestion, querying and data compression.

- **[Simplified Solution](https://tdengine.com/tdengine/simplified-time-series-data-solution/)**: Through built-in caching, stream processing and data subscription features, TDengine provides a simplified solution for time-series data processing. It reduces system design complexity and operation costs significantly.

- **[Cloud Native](https://tdengine.com/tdengine/cloud-native-time-series-database/)**: Through native distributed design, sharding and partitioning, separation of compute and storage, RAFT, support for kubernetes deployment and full observability, TDengine is a cloud native Time-Series Database and can be deployed on public, private or hybrid clouds.

- **[Ease of Use](https://tdengine.com/tdengine/easy-time-series-data-platform/)**: For administrators, TDengine significantly reduces the effort to deploy and maintain. For developers, it provides a simple interface, simplified solution and seamless integrations for third party tools. For data users, it gives easy data access. 

- **[Easy Data Analytics](https://tdengine.com/tdengine/time-series-data-analytics-made-easy/)**: Through super tables, storage and compute separation, data partitioning by time interval, pre-computation and other means, TDengine makes it easy to explore, format, and get access to data in a highly efficient way. 

- **[Open Source](https://tdengine.com/tdengine/open-source-time-series-database/)**: TDengine’s core modules, including cluster feature, are all available under open source licenses. It has gathered 19.9k stars on GitHub. There is an active developer community, and over 139k running instances worldwide.

For a full list of TDengine competitive advantages, please [check here](https://tdengine.com/tdengine/). The easiest way to experience TDengine is through [TDengine Cloud](https://cloud.tdengine.com). 

# Documentation

For user manual, system design and architecture, please refer to [TDengine Documentation](https://docs.tdengine.com) ([TDengine 文档](https://docs.taosdata.com))

# Building

At the moment, TDengine server supports running on Linux/Windows/macOS systems. Any application can also choose the RESTful interface provided by taosAdapter to connect the taosd service . TDengine supports X64/ARM64 CPU, and it will support MIPS64, Alpha64, ARM32, RISC-V and other CPU architectures in the future.

You can choose to install through source code, [container](https://docs.tdengine.com/get-started/docker/), [installation package](https://docs.tdengine.com/get-started/package/) or [Kubernetes](https://docs.tdengine.com/deployment/k8s/). This quick guide only applies to installing from source.

TDengine provide a few useful tools such as taosBenchmark (was named taosdemo) and taosdump. They were part of TDengine. By default, TDengine compiling does not include taosTools. You can use `cmake .. -DBUILD_TOOLS=true` to make them be compiled with TDengine.

To build TDengine, use [CMake](https://cmake.org/) 3.0.2 or higher versions in the project directory.

## Install build tools

### Ubuntu 18.04 and above or Debian

```bash
sudo apt-get install -y gcc cmake build-essential git libssl-dev
```

#### Install build dependencies for taosTools

To build the [taosTools](https://github.com/taosdata/taos-tools) on Ubuntu/Debian, the following packages need to be installed.

```bash
sudo apt install build-essential libjansson-dev libsnappy-dev liblzma-dev libz-dev zlib1g pkg-config
```

### CentOS 7.9

```bash
sudo yum install epel-release
sudo yum update
sudo yum install -y gcc gcc-c++ make cmake3 git openssl-devel
sudo ln -sf /usr/bin/cmake3 /usr/bin/cmake
```

### CentOS 8 & Fedora

```bash
sudo dnf install -y gcc gcc-c++ make cmake epel-release git openssl-devel
```

#### Install build dependencies for taosTools on CentOS

#### CentOS 7.9

```
sudo yum install -y zlib-devel zlib-static xz-devel snappy-devel jansson jansson-devel pkgconfig libatomic libatomic-static libstdc++-static openssl-devel
```

#### CentOS 8/Rocky Linux

```
sudo yum install -y epel-release
sudo yum install -y dnf-plugins-core
sudo yum config-manager --set-enabled powertools
sudo yum install -y zlib-devel zlib-static xz-devel snappy-devel jansson jansson-devel pkgconfig libatomic libatomic-static libstdc++-static openssl-devel
```

Note: Since snappy lacks pkg-config support (refer to [link](https://github.com/google/snappy/pull/86)), it leads a cmake prompt libsnappy not found. But snappy still works well.

If the PowerTools installation fails, you can try to use:

```
sudo yum config-manager --set-enabled powertools
```

### macOS

```
brew install argp-standalone pkgconfig
```

### Setup golang environment

TDengine includes a few components like taosAdapter developed by Go language. Please refer to golang.org official documentation for golang environment setup.

Please use version 1.14+. For the user in China, we recommend using a proxy to accelerate package downloading.

```
go env -w GO111MODULE=on
go env -w GOPROXY=https://goproxy.cn,direct
```

The default will not build taosAdapter, but you can use the following command to build taosAdapter as the service for RESTful interface.

```
cmake .. -DBUILD_HTTP=false
```

### Setup rust environment

TDengine includes a few components developed by Rust language. Please refer to rust-lang.org official documentation for rust environment setup.

## Get the source codes

First of all, you may clone the source codes from github:

```bash
git clone https://github.com/taosdata/TDengine.git
cd TDengine
```

You can modify the file ~/.gitconfig to use ssh protocol instead of https for better download speed. You will need to upload ssh public key to GitHub first. Please refer to GitHub official documentation for detail.

```
[url "git@github.com:"]
    insteadOf = https://github.com/
```

## Special Note

[JDBC Connector](https://github.com/taosdata/taos-connector-jdbc)， [Go Connector](https://github.com/taosdata/driver-go)，[Python Connector](https://github.com/taosdata/taos-connector-python)，[Node.js Connector](https://github.com/taosdata/taos-connector-node)，[C# Connector](https://github.com/taosdata/taos-connector-dotnet) ，[Rust Connector](https://github.com/taosdata/taos-connector-rust) and [Grafana plugin](https://github.com/taosdata/grafanaplugin) has been moved to standalone repository.

## Build TDengine

### On Linux platform

You can run the bash script `build.sh` to build both TDengine and taosTools including taosBenchmark and taosdump as below:

```bash
./build.sh
```

It equals to execute following commands:

```bash
mkdir debug
cd debug
cmake .. -DBUILD_TOOLS=true
make
```

You can use Jemalloc as memory allocator instead of glibc:

```
apt install autoconf
cmake .. -DJEMALLOC_ENABLED=true
```

TDengine build script can detect the host machine's architecture on X86-64, X86, arm64 platform.
You can also specify CPUTYPE option like aarch64 too if the detection result is not correct:

aarch64:

```bash
cmake .. -DCPUTYPE=aarch64 && cmake --build .
```

### On Windows platform

If you use the Visual Studio 2013, please open a command window by executing "cmd.exe".
Please specify "amd64" for 64 bits Windows or specify "x86" for 32 bits Windows when you execute vcvarsall.bat.

```cmd
mkdir debug && cd debug
"C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" < amd64 | x86 >
cmake .. -G "NMake Makefiles"
nmake
```

If you use the Visual Studio 2019 or 2017:

please open a command window by executing "cmd.exe".
Please specify "x64" for 64 bits Windows or specify "x86" for 32 bits Windows when you execute vcvarsall.bat.

```cmd
mkdir debug && cd debug
"c:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" < x64 | x86 >
cmake .. -G "NMake Makefiles"
nmake
```

Or, you can simply open a command window by clicking Windows Start -> "Visual Studio < 2019 | 2017 >" folder -> "x64 Native Tools Command Prompt for VS < 2019 | 2017 >" or "x86 Native Tools Command Prompt for VS < 2019 | 2017 >" depends what architecture your Windows is, then execute commands as follows:

```cmd
mkdir debug && cd debug
cmake .. -G "NMake Makefiles"
nmake
```

### On macOS platform

Please install XCode command line tools and cmake. Verified with XCode 11.4+ on Catalina and Big Sur.

```shell
mkdir debug && cd debug
cmake .. && cmake --build .
```

# Installing

## On Linux platform

After building successfully, TDengine can be installed by

```bash
sudo make install
```

Users can find more information about directories installed on the system in the [directory and files](https://docs.tdengine.com/reference/directory/) section.

Installing from source code will also configure service management for TDengine.Users can also choose to [install from packages](https://docs.tdengine.com/get-started/package/) for it.

To start the service after installation, in a terminal, use:

```bash
sudo systemctl start taosd
```

Then users can use the TDengine CLI to connect the TDengine server. In a terminal, use:

```bash
taos
```

If TDengine CLI connects the server successfully, welcome messages and version info are printed. Otherwise, an error message is shown.

## On Windows platform

After building successfully, TDengine can be installed by:

```cmd
nmake install
```


## On macOS platform

After building successfully, TDengine can be installed by:

```bash
sudo make install
```

Users can find more information about directories installed on the system in the [directory and files](https://docs.tdengine.com/reference/directory/) section.

Installing from source code will also configure service management for TDengine.Users can also choose to [install from packages](https://docs.tdengine.com/get-started/package/) for it.

To start the service after installation, double-click the /applications/TDengine to start the program, or in a terminal, use:

```bash
launchctl start com.tdengine.taosd
```

Then users can use the TDengine CLI to connect the TDengine server. In a terminal, use:

```bash
taos
```

If TDengine CLI connects the server successfully, welcome messages and version info are printed. Otherwise, an error message is shown.

## Quick Run

If you don't want to run TDengine as a service, you can run it in current shell. For example, to quickly start a TDengine server after building, run the command below in terminal: (We take Linux as an example, command on Windows will be `taosd.exe`)

```bash
./build/bin/taosd -c test/cfg
```

In another terminal, use the TDengine CLI to connect the server:

```bash
./build/bin/taos -c test/cfg
```

option "-c test/cfg" specifies the system configuration file directory.

# Try TDengine

It is easy to run SQL commands from TDengine CLI which is the same as other SQL databases.

```sql
CREATE DATABASE demo;
USE demo;
CREATE TABLE t (ts TIMESTAMP, speed INT);
INSERT INTO t VALUES('2019-07-15 00:00:00', 10);
INSERT INTO t VALUES('2019-07-15 01:00:00', 20);
SELECT * FROM t;
          ts          |   speed   |
===================================
 19-07-15 00:00:00.000|         10|
 19-07-15 01:00:00.000|         20|
Query OK, 2 row(s) in set (0.001700s)
```

# Developing with TDengine

## Official Connectors

TDengine provides abundant developing tools for users to develop on TDengine. Follow the links below to find your desired connectors and relevant documentation.

- [Java](https://docs.tdengine.com/reference/connector/java/)
- [C/C++](https://docs.tdengine.com/reference/connector/cpp/)
- [Python](https://docs.tdengine.com/reference/connector/python/)
- [Go](https://docs.tdengine.com/reference/connector/go/)
- [Node.js](https://docs.tdengine.com/reference/connector/node/)
- [Rust](https://docs.tdengine.com/reference/connector/rust/)
- [C#](https://docs.tdengine.com/reference/connector/csharp/)
- [RESTful API](https://docs.tdengine.com/reference/rest-api/)

# Contribute to TDengine

Please follow the [contribution guidelines](CONTRIBUTING.md) to contribute to the project.

# Join the TDengine Community

For more information about TDengine, you can follow us on social media and join our Discord server:

- [Discord](https://discord.com/invite/VZdSuUg4pS)
- [Twitter](https://twitter.com/TDengineDB)
- [LinkedIn](https://www.linkedin.com/company/tdengine/)
- [YouTube](https://www.youtube.com/@tdengine)
