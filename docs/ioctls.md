# Hailo PCIe IOCTL 분석 노트

최신화 일자: 2026-06-30

목적: Hailo8/NNC 보드에서 AI 추론과 관련된 ioctl, VDMA/FW control 경로, multi-process service 구조를 빠르게 파악하기 위한 코드 기준 정리.

## 0. 현재 기준

분석 대상:

| 구분 | 경로 |
|---|---|
| Kernel driver | `hailort-drivers/linux/pcie`, `hailort-drivers/linux/vdma`, `hailort-drivers/common` |
| User library | `hailort/hailort/libhailort` |
| HailoRT service | `hailort/hailort/hailort_service` |
| 보드 타입 | `accelerator_type = 0`, 즉 `HAILO_ACCELERATOR_TYPE_NNC` |

소스 트리 기준:

| repo | 기준 branch/commit | 상태 |
|---|---:|---|
| `hailort` | `origin/hailo8 = upstream/hailo8 = 63adffec`, `origin/linux = be31c06e` | 4.24.0 기반 Linux-only 정리 |
| `hailort-drivers` | `origin/hailo8 = upstream/hailo8 = 2789e0f`, `origin/linux = 1a0c0d7` | 4.24.0 기반 Linux-only 정리 |

보드에서 최근 확인한 loaded module:

```text
/sys/module/hailo_pci/version    = 4.24.0
/sys/module/hailo_pci/srcversion = FCEE2316295B5CDE0D84FD9
dmesg = hailo: Init module. driver version 4.24.0
```

## 1. 핵심 결론

NNC 보드에서 AI 추론 관련 ioctl은 크게 두 축이다.

| 축 | 핵심 ioctl | 역할 |
|---|---|---|
| 제어/설정 | `HAILO_FW_CONTROL` | FW control protocol을 통해 network group, context switch, infer state를 설정 |
| 데이터 이동 | `HAILO_VDMA_*`, 특히 `HAILO_VDMA_LAUNCH_TRANSFER` | host/device 사이 DMA 전송. runtime tensor I/O와 config/model context 준비에 모두 관여 |

요약:

```text
추론 설정/시작/상태 전환
  -> HAILO_FW_CONTROL
  -> 내부 CONTROL_PROTOCOL opcode로 실제 의미 결정

input/output tensor DMA
  -> HAILO_VDMA_LAUNCH_TRANSFER 중심의 boundary channel 경로

model context / weights / config
  -> VDMA buffer/descriptor/config channel 준비
  -> HAILO_FW_CONTROL로 전달된 context-switch action을 FW/Core CPU가 실행
```

`nnc.c`의 나머지 NNC ioctl은 보조 성격이다.

| ioctl | 성격 |
|---|---|
| `HAILO_READ_NOTIFICATION` | FW/D2H event 수신. 추론 에러/상태 관찰에 간접 관련 |
| `HAILO_DISABLE_NOTIFICATION` | notification wait 해제 |
| `HAILO_READ_LOG` | FW log 읽기 |

## 2. Device open과 per-open context

`/dev/hailo0` 같은 device file을 `open()`하면 두 층의 객체가 관여한다.

```text
userspace open("/dev/hailo0")
-> VFS가 struct file 생성
-> hailo_pcie_fops_open(inode, filp)
-> minor 번호로 기존 struct hailo_pcie_board 검색
-> board ref_count 증가
-> filp->private_data = pBoard
-> kzalloc(struct hailo_file_context)
-> context->filp = filp
-> hailo_vdma_file_context_init(&context->vdma_context)
-> board->open_files_list에 context 등록
-> context->is_valid = true
-> NNC면 notification wait 객체 추가
```

핵심은 `filp->private_data`에 per-open context가 아니라 `struct hailo_pcie_board *`가 들어간다는 점이다.

```text
filp->private_data = board

ioctl/mmap/release:
  board = filp->private_data
  lock(board->mutex)
  context = find_file_context(board, filp)
```

이유는 `struct hailo_pcie_board`가 fops 경로의 root object이기 때문이다. `ioctl`, `mmap`, `release`는 per-open context보다 먼저 장치 전체 상태를 필요로 한다.

| board 상태 | 사용 목적 |
|---|---|
| `board->pDev` | PCI device가 아직 유효한지 확인 |
| `board->mutex` | board 공통 상태 보호 |
| `board->pcie_resources.accelerator_type` | NNC/SOC ioctl dispatch |
| `board->vdma` | file handle들이 공유하는 VDMA controller |
| `board->pcie_resources` | FW access, BAR/resource, accelerator 정보 |
| `board->interrupts_enabled`, `board->ref_count` | interrupt와 open reference 관리 |

관계는 `1 board : N file context`다.

```text
1 physical Hailo board
  -> struct hailo_pcie_board 1개
  -> board->open_files_list
      -> struct hailo_file_context A
      -> struct hailo_file_context B
      -> struct hailo_file_context C
```

`struct hailo_file_context`는 fd별 상태를 담는다.

```c
struct hailo_file_context {
    struct list_head open_files_list;
    struct file *filp;
    struct hailo_vdma_file_context vdma_context;
    bool is_valid;
    u32 soc_used_channels_bitmap;
};
```

per-open context의 주요 상태:

| 상태 | 의미 |
|---|---|
| `filp` | 현재 open instance와 context를 매칭하기 위한 key |
| `vdma_context` | 이 fd가 map/create한 VDMA buffer, descriptor list, enabled channel bitmap 등 |
| `is_valid` | remove/suspend 등에서 fd별 invalidation 처리 |
| `soc_used_channels_bitmap` | SOC 경로에서 이 fd가 사용한 channel 추적 |
| NNC notification wait object | FW/D2H notification 수신을 file handle 단위로 분리 |

관련 코드:

| 항목 | 위치 |
|---|---|
| `struct hailo_file_context` 정의 | `hailort-drivers/linux/pcie/src/pcie.h:68` |
| board lookup/ref_count 증가 | `hailort-drivers/linux/pcie/src/pcie.c:134` |
| file context 생성 | `hailort-drivers/linux/pcie/src/fops.c:52` |
| `filp->private_data = pBoard` | `hailort-drivers/linux/pcie/src/fops.c:104` |
| ioctl에서 context lookup | `hailort-drivers/linux/pcie/src/fops.c:423` |
| mmap에서 context lookup | `hailort-drivers/linux/pcie/src/fops.c:506` |
| release에서 context finalize | `hailort-drivers/linux/pcie/src/fops.c:181` |
| NNC notification wait 추가 | `hailort-drivers/linux/pcie/src/nnc.c:291` |
| VDMA per-file context init/finalize | `hailort-drivers/linux/vdma/vdma.c:99`, `vdma.c:120` |

## 3. Multi-process 구조와 HailoRT service

Kernel driver 자체는 multi-open을 전제로 설계되어 있다. `open()` 주석에도 여러 process가 device를 열 수 있고 ref_count로 관리한다고 나온다.

하지만 이것만으로 "여러 process가 NPU compute를 아무 조율 없이 동시에 병렬 사용 가능"이라는 뜻은 아니다.

계층을 나누면 다음과 같다.

```text
kernel driver
  multi-open 허용
  fd별 VDMA/notification/resource tracking

hailort_service
  multi-process용 user-space daemon
  VDevice / ConfiguredNetworkGroup / VStream resource 중앙 관리
  scheduler RPC 제공

libhailort scheduler / FW
  실제 network group scheduling
  context switch
  infer request 조율
```

`hailort_service`는 단순 background thread가 아니라 systemd daemon process다.

| 항목 | 위치 |
|---|---|
| systemd unit | `hailort/hailort/hailort_service/hailort.service:1` |
| daemon main | `hailort/hailort/hailort_service/unix/hailort_service.cpp:84` |
| gRPC service 구현 | `hailort/hailort/hailort_service/hailort_rpc_service.cpp` |
| keep-alive thread | `hailort/hailort/hailort_service/hailort_rpc_service.cpp:39` |
| service resource manager | `hailort/hailort/hailort_service/service_resource_manager.hpp:22` |

Service mode 흐름:

```text
client process
  -> libhailort VDeviceClient
  -> gRPC
  -> hailort_service
  -> VDevice::create()
  -> handle 반환
```

핵심 코드:

| 동작 | 위치 |
|---|---|
| `multi_process_service` API field | `hailort/hailort/libhailort/include/hailo/hailort.h:423` |
| `VDevice::create()`에서 service path 분기 | `hailort/hailort/libhailort/src/vdevice/vdevice.cpp:542` |
| service client 생성 | `hailort/hailort/libhailort/src/vdevice/vdevice.cpp:343` |
| service 통신 초기화 및 version check | `hailort/hailort/libhailort/src/service/rpc_client_utils.hpp:127` |
| service 측 `VDevice_create` RPC | `hailort/hailort/hailort_service/hailort_rpc_service.cpp:224` |
| scheduler timeout/threshold/priority RPC | `hailort/hailort/hailort_service/hailort_rpc_service.cpp:1382` |
| libhailort scheduler worker thread | `hailort/hailort/libhailort/src/vdevice/scheduler/scheduler.cpp:482` |

Service 활성화 여부에 따른 동작:

| 조건 | 결과 |
|---|---|
| `multi_process_service=false` | service 없이 direct mode로 동작 가능 |
| `multi_process_service=true`, service 실행 중 | service mode로 VDevice/CNG/VStream resource를 중앙 관리 |
| `multi_process_service=true`, service 미실행 | gRPC 연결/version check 실패로 VDevice 생성 실패 가능 |

즉 HailoRT가 의도한 multi-process NPU 사용 방식은 `hailort_service`를 켠 service mode다. Kernel driver의 `open_files_list`는 그 하부에서 여러 fd/process의 resource를 안전하게 추적하기 위한 기반이다.

## 4. ioctl 진입 흐름

User space에서 ioctl을 호출하면 다음 경로로 들어간다.

```text
libhailort
  HailoRTDriver::run_ioctl()
  -> run_hailo_ioctl()
  -> ioctl(fd, ioctl_code, param)

kernel driver
  hailo_pcie_fops_unlockedioctl()
  -> switch (_IOC_TYPE(cmd))
     - HAILO_GENERAL_IOCTL_MAGIC
     - HAILO_VDMA_IOCTL_MAGIC
     - HAILO_SOC_IOCTL_MAGIC
     - HAILO_NNC_IOCTL_MAGIC
```

관련 코드:

| 항목 | 위치 |
|---|---|
| user ioctl wrapper | `hailort/hailort/libhailort/src/vdma/driver/hailort_driver.cpp:732` |
| Linux syscall wrapper | `hailort/hailort/libhailort/src/vdma/driver/os/posix/linux/driver_os_specific.cpp:180` |
| kernel fops entry | `hailort-drivers/linux/pcie/src/fops.c:423` |
| file operations 등록 | `hailort-drivers/linux/pcie/src/pcie.c:1457` |

## 5. NNC ioctl

NNC ioctl 정의:

| 항목 | 위치 |
|---|---|
| NNC ioctl code enum | `hailort-drivers/common/hailo_ioctl_common.h:475` |
| NNC ioctl macro | `hailort-drivers/common/hailo_ioctl_common.h:487` |
| kernel switch | `hailort-drivers/linux/pcie/src/nnc.c:257` |

현재 NNC switch:

```c
case HAILO_FW_CONTROL:
    return hailo_fw_control(...);
case HAILO_READ_NOTIFICATION:
    return hailo_read_notification_ioctl(...);
case HAILO_DISABLE_NOTIFICATION:
    return hailo_disable_notification(...);
case HAILO_READ_LOG:
    return hailo_read_log_ioctl(...);
default:
    return -ENOTTY;
```

NNC ioctl 의미:

| ioctl | 구현 | 추론 관련도 | 설명 |
|---|---:|---:|---|
| `HAILO_FW_CONTROL` | `nnc.c:82` | 높음 | FW control protocol 운반. context switch, network config, infer state 제어 |
| `HAILO_READ_NOTIFICATION` | `nnc.c:164` | 중간 | FW/D2H event 수신 |
| `HAILO_DISABLE_NOTIFICATION` | `nnc.c:217` | 낮음 | notification wait 해제 |
| `HAILO_READ_LOG` | `nnc.c:235` | 낮음 | FW log 읽기 |
| `HAILO_RESET_NN_CORE` | 정의만 확인 | 주의 | 현재 `nnc.c` switch에는 case 없음 |
| `HAILO_WRITE_ACTION_LIST` | 정의만 확인 | 주의 | 현재 `nnc.c` switch에는 case 없음 |

## 6. `HAILO_FW_CONTROL`

`HAILO_FW_CONTROL`은 ioctl 하나지만 실제 기능은 내부 `CONTROL_PROTOCOL__request_t.opcode`로 갈린다.

Driver-side 처리:

```text
copy_from_user(struct hailo_fw_control)
-> hailo_fw_control_preprocess()
-> hailo_pcie_write_firmware_control()
-> wait_for_completion_interruptible_timeout()
-> hailo_pcie_read_firmware_control()
-> copy_to_user()
```

`hailo_pcie_write_firmware_control()`은 opcode를 보고 target CPU를 고른 뒤 FW access region에 request를 쓰고 ready bit를 올린다.

| 항목 | 위치 |
|---|---|
| NNC ioctl dispatch | `hailort-drivers/linux/pcie/src/nnc.c:257` |
| FW control ioctl body | `hailort-drivers/linux/pcie/src/nnc.c:82` |
| opcode 읽기 | `hailort-drivers/common/pcie_common.c:463` |
| opcode 범위 체크 | `hailort-drivers/common/pcie_common.c:471` |
| request write | `hailort-drivers/common/pcie_common.c:479` |
| CPU target 선택 | `hailort-drivers/common/pcie_common.c:485` |
| ready bit write | `hailort-drivers/common/pcie_common.c:487` |
| response read | `hailort-drivers/common/pcie_common.c:492` |

추론 관련 주요 opcode:

| opcode | target | 용도 |
|---|---|---|
| `HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_NETWORK_GROUP_HEADER` | `CPU_ID_CORE_CPU` | network group header 전달 |
| `HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_CONTEXT_INFO` | `CPU_ID_CORE_CPU` | context/action-list 정보 전달 |
| `HAILO_CONTROL_OPCODE_DOWNLOAD_CONTEXT_ACTION_LIST` | `CPU_ID_CORE_CPU` | context action list 다운로드 |
| `HAILO_CONTROL_OPCODE_CHANGE_CONTEXT_SWITCH_STATUS` | `CPU_ID_CORE_CPU` | context switch state machine enable/reset |
| `HAILO_CONTROL_OPCODE_SET_DATAFLOW_INTERRUPT` | `CPU_ID_CORE_CPU` | dataflow interrupt 설정 |
| `HAILO_CONTROL_OPCODE_CHANGE_HW_INFER_STATUS` | `CPU_ID_CORE_CPU` | HW-only infer start/stop |

관련 정의:

- `hailort-drivers/common/control_protocol.h:79`
- `hailort-drivers/common/control_protocol.h:111`
- `hailort-drivers/common/control_protocol.h:153`

libhailort 호출 위치:

| 함수 | 의미 | 위치 |
|---|---|---|
| `Control::context_switch_set_network_group_header()` | network group header 설정 | `hailort/hailort/libhailort/src/device_common/control.cpp:1111` |
| `Control::context_switch_set_context_info_chunk()` | context info 설정 | `hailort/hailort/libhailort/src/device_common/control.cpp:1126` |
| `Control::download_context_action_list()` | action list 다운로드 | `hailort/hailort/libhailort/src/device_common/control.cpp:1251` |
| `Control::enable_core_op()` | context switch enable | `hailort/hailort/libhailort/src/device_common/control.cpp:1310` |
| `Control::reset_context_switch_state_machine()` | context switch reset | `hailort/hailort/libhailort/src/device_common/control.cpp:1317` |
| `Control::start_hw_only_infer()` | HW-only infer start | `hailort/hailort/libhailort/src/device_common/control.cpp:1729` |
| `Control::stop_hw_only_infer()` | HW-only infer stop | `hailort/hailort/libhailort/src/device_common/control.cpp:1738` |

Network configure 예:

```text
resource_manager.cpp
-> Control::context_switch_set_network_group_header()
-> Control::context_switch_set_context_info()
-> Device::fw_interact()
-> HailoRTDriver::fw_control()
-> HAILO_FW_CONTROL ioctl
```

관련 위치: `hailort/hailort/libhailort/src/core_op/resource_manager/resource_manager.cpp:670`, `resource_manager.cpp:673`

## 7. VDMA ioctl

VDMA는 tensor 전용 API가 아니라 host/device 사이 DMA channel과 descriptor 기반 전송 인프라다.

VDMA ioctl 정의 및 dispatch:

| 항목 | 위치 |
|---|---|
| VDMA ioctl enum | `hailort-drivers/common/hailo_ioctl_common.h:430` |
| VDMA ioctl macro | `hailort-drivers/common/hailo_ioctl_common.h:452` |
| VDMA kernel dispatcher | `hailort-drivers/linux/vdma/vdma.c:180` |

주요 VDMA ioctl:

| ioctl | libhailort | kernel | 추론 관련도 | 설명 |
|---|---:|---:|---:|---|
| `HAILO_VDMA_BUFFER_MAP` | `hailort_driver.cpp:743` | `vdma.c:192` | 높음 | user buffer 또는 dmabuf를 DMA 가능한 buffer로 map |
| `HAILO_VDMA_BUFFER_SYNC` | `hailort_driver.cpp:527` | `vdma.c:196` | 높음 | CPU/device cache sync |
| `HAILO_DESC_LIST_CREATE` | `hailort_driver.cpp:769` | `vdma.c:198` | 높음 | descriptor list 생성 |
| `HAILO_DESC_LIST_PROGRAM` | `hailort_driver.cpp:539` | `vdma.c:202` | 높음 | descriptor list에 buffer/channel 정보 program |
| `HAILO_VDMA_ENABLE_CHANNELS` | `hailort_driver.cpp:291` | `vdma.c:184` | 높음 | VDMA channel enable |
| `HAILO_VDMA_LAUNCH_TRANSFER` | `hailort_driver.cpp:566` | `vdma.c:214` | 매우 높음 | VDMA transfer 시작 |
| `HAILO_VDMA_INTERRUPTS_WAIT` | `hailort_driver.cpp:360` | `vdma.c:188` | 매우 높음 | transfer 완료 interrupt 대기 |
| `HAILO_VDMA_INTERRUPTS_READ_TIMESTAMPS` | `hailort_driver.cpp:371` | `vdma.c:190` | 중간 | timestamp/profiling |
| `HAILO_VDMA_BUFFER_UNMAP` | `hailort_driver.cpp:760` | `vdma.c:194` | 중간 | mapped buffer 해제 |
| `HAILO_DESC_LIST_RELEASE` | `hailort_driver.cpp:784` | `vdma.c:200` | 중간 | descriptor list 해제 |

## 8. Boundary channel vs Config channel

VDMA ioctl은 input/output tensor 전용이 아니다. 두 경로가 같이 올라간다.

| 구분 | 대상 | VDMA 사용 방식 | 대표 코드 |
|---|---|---|---|
| Boundary channel | input/output tensor | stream read/write 때 userspace가 `HAILO_VDMA_LAUNCH_TRANSFER`를 반복 호출 | `vdma_stream.cpp:194`, `boundary_channel.cpp:268` |
| Config channel | model context, NN configuration, CCW, shared weights/aligned CCWs | VDMA descriptor/channel을 준비하고 FW context-switch action이 config channel을 fetch/activate | `config_buffer.cpp:7`, `context_switch_actions.cpp:119` |

Boundary channel runtime path:

```text
input/output stream write/read
-> BoundaryChannel::launch_transfer()
-> HailoRTDriver::launch_transfer()
-> HAILO_VDMA_LAUNCH_TRANSFER
-> hailo_vdma_launch_transfer_ioctl()
```

Config channel configure path:

```text
HEF parsing / network configure
-> ConfigBufferInfo 생성
-> config channel id 할당
-> config buffer 또는 descriptor list 준비
   - HAILO_VDMA_BUFFER_MAP
   - HAILO_DESC_LIST_CREATE
   - HAILO_DESC_LIST_PROGRAM
-> context-switch action list 생성
   - ACTIVATE_CFG_CHANNEL
   - FETCH_CFG_CHANNEL_DESCRIPTORS
   - FETCH_CCW_BURSTS
   - DEACTIVATE_CFG_CHANNEL
-> HAILO_FW_CONTROL로 FW에 context/action 정보 전달
-> FW/Core CPU가 config channel fetch/activate 수행
```

strace 해석 기준:

| 관찰 | 해석 |
|---|---|
| `HAILO_VDMA_LAUNCH_TRANSFER`가 대량 반복 | 대부분 runtime input/output tensor boundary traffic |
| configure 초반의 `BUFFER_MAP`, `DESC_LIST_CREATE`, `DESC_LIST_PROGRAM` | tensor buffer 준비일 수도 있고 config/model context 준비일 수도 있음 |
| context-switch 계열 `HAILO_FW_CONTROL`과 VDMA descriptor 준비가 함께 보임 | model context, NN configuration, CCW/shared weights 준비 경로 의심 |

Config channel 관련 코드:

| 항목 | 위치 |
|---|---|
| config buffer 설명 | `hailort/hailort/libhailort/src/core_op/resource_manager/config_buffer.cpp:7` |
| `ConfigBufferInfo`와 shared weights DMA transfer | `hailort/hailort/libhailort/src/hef/core_op_metadata.hpp:38` |
| config channel id 할당 | `hailort/hailort/libhailort/src/core_op/resource_manager/resource_manager.cpp:270` |
| app header에 config channel 정보 삽입 | `hailort/hailort/libhailort/src/core_op/resource_manager/resource_manager.cpp:403` |
| aligned CCW descriptor program | `hailort/hailort/libhailort/src/core_op/resource_manager/config_buffer.cpp:49`, `config_buffer.cpp:98` |
| config channel action | `hailort/hailort/libhailort/src/hef/context_switch_actions.cpp:119`, `context_switch_actions.cpp:287`, `context_switch_actions.cpp:317` |
| action list에 config action 삽입 | `hailort/hailort/libhailort/src/core_op/resource_manager/resource_manager_builder.cpp:807`, `resource_manager_builder.cpp:830` |

## 9. Model context switch 실행 주체

Model context switch는 Host가 준비하고 trigger하지만, 실제 실행은 FW/Core CPU 주도다.

| 주체 | 역할 |
|---|---|
| `libhailort` | HEF parsing, context info/action list/config buffer/VDMA descriptor 준비 |
| kernel driver | `HAILO_FW_CONTROL` request를 FW access 영역에 쓰고 target CPU ready bit를 올림 |
| FW/Core CPU | context switch state machine 실행, action list 해석, config channel fetch/activate |
| VDMA HW | 지정된 channel/descriptor 기반으로 실제 DMA 수행 |
| NN compute core | 실제 tensor 연산 수행 |

단순 흐름:

```text
libhailort
  HEF/context/config/action 준비
  -> HAILO_FW_CONTROL

kernel driver
  FW access memory에 request write
  -> Core CPU ready bit raise

FW/Core CPU
  context switch state machine 실행
  -> action list 해석
  -> config channel activate/fetch
  -> NN context 전환

VDMA HW / NN compute core
  데이터 이동 및 tensor 연산 수행
```

즉 driver가 model context switch를 직접 수행하지 않는다. driver는 transport에 가깝고, context-switch state machine은 Hailo 칩 내부 Core CPU firmware가 실행한다.

## 10. FW, App CPU, Core CPU, NN core 관계

| 용어 | 의미 |
|---|---|
| Host CPU | Linux, userspace/libhailort, kernel driver가 실행되는 CPU |
| Hailo/NPU device | PCIe로 연결된 Hailo accelerator 칩 |
| App CPU | Hailo 칩 내부의 management/control plane embedded CPU |
| Core CPU | Hailo 칩 내부의 NN core/context-switch/dataflow 제어 embedded CPU |
| NN compute core | tensor 연산을 실제 수행하는 accelerator hardware |
| FW | App CPU/Core CPU에서 실행되는 firmware image/program |

Firmware image도 app/core 쪽으로 분리된다.

| 항목 | 위치 |
|---|---|
| app firmware write | `hailort-drivers/common/pcie_common.c:648` |
| core firmware write | `hailort-drivers/common/pcie_common.c:664` |
| firmware load 시 app/core image 처리 | `hailort-drivers/common/pcie_common.c:781`, `pcie_common.c:783` |

Control request도 opcode별로 App CPU/Core CPU target을 구분한다.

| 항목 | 위치 |
|---|---|
| opcode별 target CPU table | `hailort-drivers/common/pcie_common.c:67` |
| App/Core ready bit mask | `hailort-drivers/common/hailo_ioctl_common.h:39` |
| opcode target에 따라 ready bit 선택 | `hailort-drivers/common/pcie_common.c:485` |
| FW log app/core debug buffer 구분 | `hailort-drivers/common/fw_operation.c:128` |

App CPU/Core CPU를 코드상 직접 master/slave로 부르지는 않는다. 더 정확한 해석은 역할 분리다.

| 관점 | 관제자에 가까운 주체 | 제어 대상 |
|---|---|---|
| 장치 관리/boot/board config | App CPU firmware | device management block |
| 추론/context switch | Core CPU firmware | NN compute core, VDMA/dataflow/config channel |
| Host 관점 | libhailort/kernel driver | App CPU 또는 Core CPU FW endpoint |

## 11. 추론 흐름 요약

```text
1. Device scan/open
   - GENERAL ioctl로 device properties 확인

2. Network/HEF configure
   - HAILO_FW_CONTROL
     - CONTEXT_SWITCH_SET_NETWORK_GROUP_HEADER
     - CONTEXT_SWITCH_SET_CONTEXT_INFO
   - VDMA descriptor/buffer 준비
     - HAILO_VDMA_BUFFER_MAP
     - HAILO_DESC_LIST_CREATE
     - HAILO_DESC_LIST_PROGRAM

3. Core op enable 또는 HW infer start
   - HAILO_FW_CONTROL
     - CHANGE_CONTEXT_SWITCH_STATUS
     - 또는 CHANGE_HW_INFER_STATUS

4. Input/output tensor transfer
   - HAILO_VDMA_ENABLE_CHANNELS
   - HAILO_VDMA_LAUNCH_TRANSFER
   - HAILO_VDMA_INTERRUPTS_WAIT
   - HAILO_VDMA_BUFFER_SYNC

5. Event/log 확인
   - HAILO_READ_NOTIFICATION
   - HAILO_READ_LOG
```

## 12. 빠른 추적 포인트

User space:

| 함수 | 위치 |
|---|---|
| `HailoRTDriver::fw_control()` | `hailort/hailort/libhailort/src/vdma/driver/hailort_driver.cpp:402` |
| `HailoRTDriver::launch_transfer()` | `hailort/hailort/libhailort/src/vdma/driver/hailort_driver.cpp:566` |
| `HailoRTDriver::vdma_interrupts_wait()` | `hailort/hailort/libhailort/src/vdma/driver/hailort_driver.cpp:360` |
| `HailoRTDriver::vdma_buffer_map_ioctl()` | `hailort/hailort/libhailort/src/vdma/driver/hailort_driver.cpp:743` |

Kernel:

| 함수 | 위치 |
|---|---|
| `hailo_pcie_fops_unlockedioctl()` | `hailort-drivers/linux/pcie/src/fops.c:423` |
| `hailo_nnc_ioctl()` | `hailort-drivers/linux/pcie/src/nnc.c:257` |
| `hailo_fw_control()` | `hailort-drivers/linux/pcie/src/nnc.c:82` |
| `hailo_vdma_ioctl()` | `hailort-drivers/linux/vdma/vdma.c:180` |
| `hailo_vdma_interrupts_wait_ioctl()` | `hailort-drivers/linux/vdma/ioctl.c:119` |
| `hailo_vdma_buffer_map_ioctl()` | `hailort-drivers/linux/vdma/ioctl.c:200` |

최종 우선순위:

1. `HAILO_FW_CONTROL`
   - network configure, context switch, infer state.
   - 내부 opcode까지 봐야 의미가 확정된다.

2. `HAILO_VDMA_LAUNCH_TRANSFER`
   - runtime input/output tensor DMA의 핵심.

3. `HAILO_VDMA_INTERRUPTS_WAIT`
   - DMA 완료 대기.

4. `HAILO_VDMA_BUFFER_MAP`, `HAILO_DESC_LIST_CREATE`, `HAILO_DESC_LIST_PROGRAM`, `HAILO_VDMA_BUFFER_SYNC`
   - DMA 준비, config channel/model context 준비, cache coherency.

5. `HAILO_READ_NOTIFICATION`
   - FW event 확인. 추론 실패/상태 이벤트 관찰에 유용.
