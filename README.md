# GTMedia HDTV Mate ATSC 3.0 macOS Driver

GTMedia HDTV Mate USB 튜너 스틱을 macOS에서 사용하기 위한 오픈소스 드라이버 프로젝트.

공식적으로 Android만 지원하는 이 디바이스를, APK 리버스 엔지니어링을 통해 macOS 네이티브 드라이버를 개발한다.

## 하드웨어 분석 결과

```
┌──────────────────────────────────────────────┐
│           GTMedia HDTV Mate USB Stick         │
│                                              │
│  ┌─────────┐    I2C    ┌──────────────────┐  │
│  │ ITE     │◄────────►│ Sony CXD6801     │  │
│  │ IT9300  │           │ ATSC 3.0 Demod   │  │
│  │ USB     │           │ + ASCOT3 Tuner   │  │
│  │ Bridge  │           └──────────────────┘  │
│  └────┬────┘                                 │
│       │ USB 2.0 High-Speed                   │
└───────┼──────────────────────────────────────┘
        │
   ┌────┴────┐
   │   Mac   │
   │ (libusb)│
   └─────────┘
```

| 구성요소 | 칩셋 | 역할 |
|---------|------|------|
| USB 브리지 | ITE IT9300 | USB ↔ I2C 변환, 펌웨어 실행, TS 멀티플렉싱, PID 필터링 |
| 디모듈레이터 | Sony CXD6801 | ATSC 3.0/1.0/J.83B 복조, ALP/TS 출력 |
| RF 튜너 | Sony ASCOT3 (내장) | RF 주파수 선택, PLL 합성, LNA |

## 리버스 엔지니어링

### 소스: Android APK

`HDTV Player_2.42_APKPure.xapk` (패키지: `com.zva3.mobile.litetvandroid`)

libatsc3 오픈소스 라이브러리 기반이나, **Sony CXD6801 PHY 드라이버는 비공개 (proprietary)**.

### 핵심 바이너리: `liba3_phy_sony.so`

**unstripped (debug_info 포함)** — 리버스 엔지니어링에 매우 유리.

| 라이브러리 | 크기 | 상태 | 역할 |
|-----------|------|------|------|
| `liba3_phy_sony.so` | 5.5 MB | **not stripped** | PHY 드라이버 (IT9300 + CXD6801) |
| `liba3_core.so` | 11 MB | not stripped | ATSC 3.0 프로토콜 스택 |
| `liba3_bridge.so` | 778 KB | not stripped | 서비스 관리 브리지 |
| `liba3_bridge_media_mmt.so` | 1.8 MB | not stripped | MMT 미디어 핸들링 |
| `liba3_bridge_media_a1ts.so` | 315 KB | not stripped | ATSC 1.0 TS 핸들링 |
| `libusb_android_sony.so` | 270 KB | not stripped | Android libusb 래퍼 |
| `libavcodec/util/swresample/swscale.so` | 각종 | stripped | FFmpeg A/V 코덱 |

### 심볼 분석 요약

`liba3_phy_sony.so`에서 추출한 **2,183개 exported 함수**:

```
469개 핵심 함수 분류:
  - Sony CXD6801 디모듈레이터:  185개 (sony_cxd6801_*)
  - CXD6801 ATSC 3.0 모니터:    22개
  - CXD6801 통합 레이어:        21개
  - ASCOT3 튜너:                12개
  - IT9300 브리지 API:          30개+
  - 브리지 커맨드 (BrCmd/Cmd):  20개
  - 드라이버 레이어 (DL_):      40개+
  - JNI 인터페이스:             8개
  - SonyPHYAndroid 클래스:      20개+
```

### 추출된 데이터

| 항목 | 크기 | 설명 |
|-----|------|------|
| IT9300 펌웨어 | 9,385 bytes | `brFirmware_codes/segments/partitions/scripts` |
| 소스 경로 | 206개 파일 | debug_info에서 원본 소스 트리 구조 확인 |

### 디바이스 초기화 흐름 (바이너리 분석)

```
SonyPHYAndroid::open(vid, pid, path)
  ├── libusb_init() → libusb_open()
  ├── USB 엔드포인트 탐색 (TX, RX, RX_TS)
  ├── libusb_claim_interface()
  │
  ├── download_bootloader_firmware()
  │   ├── BrCmd_loadFirmware()  ← brFirmware_codes[] 업로드
  │   └── IT9300_reboot()
  │
  ├── internal_it930x_initialize()
  │   ├── IT9300_initialize()
  │   ├── internal_getEEPROMConfig()  ← 디바이스 설정 읽기
  │   └── internal_get_rx_id()        ← 디모듈레이터 종류 판별
  │
  ├── DRV_CXD6801_initialize()
  │   ├── drvi2c_cxd6801_ite_Initialize()  ← I2C 브리지 설정
  │   ├── sony_cxd6801_demod_Create()
  │   ├── sony_cxd6801_demod_Initialize()  ← 레지스터 초기화 시퀀스
  │   ├── sony_cxd6801_ascot3_Create()
  │   └── sony_cxd6801_ascot3_Initialize() ← 튜너 초기화
  │
  └── 4개 스레드 시작:
      ├── captureThread  ← USB bulk read (ENDPOINT_RX_TS)
      ├── processThread  ← TLV/ALP 파싱
      ├── statusThread   ← 신호 품질 모니터링
      └── consumerThread ← 데이터 소비
```

### 튜닝 흐름

```
nativeTune(frequency_khz, bandwidth, plp_id)
  ├── sony_cxd6801_ascot3_Tune()        ← RF PLL 주파수 설정
  ├── sony_cxd6801_demod_atsc3_Tune()   ← 디모듈레이터 ATSC3 모드
  ├── sony_cxd6801_demod_TuneEnd()      ← 수신 시작
  └── DRV_CXD6801_acquireChannelPlp()   ← PLP 선택 + 잠금 대기
```

## macOS 드라이버 (`hdtvmate-macos/`)

### 아키텍처

```
┌─────────────────────────────────────────────────┐
│              macOS CLI / SwiftUI App             │
├─────────────────────────────────────────────────┤
│         libhdtvmate_core.a (C 라이브러리)         │
│  ┌──────────┐  ┌────────────┐  ┌─────────────┐ │
│  │ USB 레이어│  │ IT9300     │  │ CXD6801     │ │
│  │ (libusb) │  │ 브리지     │  │ 디모듈레이터│ │
│  └──────────┘  └────────────┘  └─────────────┘ │
│  ┌──────────┐  ┌────────────┐  ┌─────────────┐ │
│  │ 채널 스캔│  │ TS 캡처    │  │ UDP/파일    │ │
│  │          │  │ (3 threads)│  │ 출력        │ │
│  └──────────┘  └────────────┘  └─────────────┘ │
└─────────────────────────────────────────────────┘
```

### 빌드

```bash
# 의존성
brew install libusb cmake

# 빌드
cd hdtvmate-macos
mkdir build && cd build
cmake ..
make
```

### CLI 도구

```bash
# 1. USB 디바이스 탐지
./hdtvmate_detect              # ITE 디바이스 자동 탐지
./hdtvmate_detect --list       # 전체 USB 디바이스 목록
./hdtvmate_detect 048D 9306    # 특정 VID/PID로 연결

# 2. 하드웨어 초기화 테스트
./hdtvmate_init                # 전체 초기화 (USB → IT9300 → CXD6801)
./hdtvmate_init <VID> <PID>    # 특정 디바이스

# 3. 채널 스캔
./hdtvmate_scan                # ATSC 3.0 + 1.0 전체 스캔
./hdtvmate_scan --atsc3        # ATSC 3.0만
./hdtvmate_scan --atsc1        # ATSC 1.0만
./hdtvmate_scan --channel 14   # 단일 채널 (CH 14 = 473 MHz)

# 4. 튜닝 + 캡처
./hdtvmate_tune 473000                      # stdout 출력 (파이프 가능)
./hdtvmate_tune --channel 14 -o output.ts   # 파일 저장
./hdtvmate_tune --channel 14 --udp 127.0.0.1:1234  # UDP 스트리밍

# VLC/mpv로 시청
vlc udp://@:1234
mpv udp://127.0.0.1:1234
```

### 프로젝트 구조

```
hdtvmate-macos/
├── CMakeLists.txt
├── include/
│   ├── hdtvmate.h              # 공통 타입, 에러 코드, 로깅
│   ├── usb_device.h            # USB 디바이스 추상화
│   ├── it9300.h                # IT9300 브리지 API
│   ├── cxd6801.h               # CXD6801 디모듈레이터 API
│   ├── channel_scan.h          # 채널 스캔 API
│   └── capture.h               # 캡처 쓰레드 + 출력
├── src/
│   ├── usb/usb_device.c        # libusb 래퍼, 자동 탐지
│   ├── bridge/
│   │   ├── br_user.c           # 플랫폼 추상화 (libusb bulk transfer)
│   │   ├── br_cmd.c            # IT9300 커맨드 프로토콜 (체크섬, 프레이밍)
│   │   ├── it9300.c            # IT9300 초기화, EEPROM, PID 필터
│   │   └── br_firmware.h       # 추출된 IT9300 펌웨어 (9,385 bytes)
│   ├── demod/
│   │   ├── cxd6801_i2c_ite.c   # I2C over IT9300 (뱅크 선택 + R/W)
│   │   ├── cxd6801.c           # 디모듈레이터 코어 (init, sleep, acquire)
│   │   ├── cxd6801_atsc3.c     # ATSC 3.0 튜닝/잠금/PLP
│   │   ├── cxd6801_monitor.c   # SNR/BER/L1/Bootstrap 모니터링
│   │   └── ascot3.c            # ASCOT3 RF 튜너 (PLL, BW 필터)
│   ├── scan/
│   │   ├── frequency_table.c   # US ATSC 주파수 테이블 (CH 2-51)
│   │   └── channel_scan.c      # 전체/단일 채널 스캔
│   ├── capture/
│   │   ├── circular_buffer.c   # 링 버퍼 (8 MB, 쓰레드 세이프)
│   │   └── capture_thread.c    # USB bulk 캡처 + TLV 처리
│   ├── output/
│   │   ├── udp_output.c        # UDP 멀티캐스트 출력
│   │   └── file_output.c       # TS 파일 저장
│   └── tools/
│       ├── detect.c            # hdtvmate_detect CLI
│       ├── init_test.c         # hdtvmate_init CLI
│       ├── scan.c              # hdtvmate_scan CLI
│       └── tune.c              # hdtvmate_tune CLI
└── tools/
    ├── extract_firmware.py     # 바이너리에서 IT9300 펌웨어 추출
    └── analyze_symbols.py      # .so 심볼 분석/분류 리포트
```

## 현재 상태

### 완료

- [x] APK 리버스 엔지니어링 (구조 분석, 네이티브 라이브러리 추출)
- [x] 하드웨어 아키텍처 파악 (ITE IT9300 + Sony CXD6801)
- [x] 2,183개 심볼 분석 및 469개 핵심 함수 분류
- [x] IT9300 펌웨어 추출 (9,385 bytes)
- [x] 소스 트리 구조 재구성 (debug_info에서 206개 파일 경로)
- [x] macOS 드라이버 프레임워크 구현 (20개 C 소스 파일)
- [x] USB 디바이스 탐지 (libusb)
- [x] IT9300 USB 커맨드 프로토콜 (체크섬, 레지스터 R/W, 펌웨어 로드)
- [x] CXD6801 I2C-over-IT9300 통신 레이어
- [x] ATSC 3.0/1.0 채널 스캔 프레임워크
- [x] 3-스레드 TS 캡처 (capture/process/status)
- [x] UDP + 파일 출력 (VLC/mpv 연동)
- [x] 4개 CLI 도구 빌드 (ARM64 macOS)

### TODO: Ghidra 디컴파일 필요

CXD6801 PHY 드라이버 소스가 비공개이므로, 아래 함수들의 레지스터 시퀀스를 Ghidra로 추출해야 함:

| 우선순위 | 함수 | 주소 | 설명 |
|---------|------|------|------|
| 1 | `sony_cxd6801_demod_Initialize` | `0x0e2698` | 디모듈레이터 초기화 레지스터 시퀀스 |
| 2 | `sony_cxd6801_demod_atsc3_Tune` | `0x0ea914` | ATSC 3.0 튜닝 레지스터 설정 |
| 3 | `sony_cxd6801_ascot3_Tune` | `0x0df178` | ASCOT3 PLL 주파수 계산 |
| 4 | `sony_cxd6801_ascot3_Initialize` | `0x0de514` | 튜너 초기화 시퀀스 |
| 5 | `DRV_CXD6801_initialize` | `0x1ae1b8` | 전체 초기화 통합 흐름 |
| 6 | `BrUser_busTx` / `BrUser_busRx` | `0x188650` / `0x1889c8` | USB 프로토콜 상세 |
| 7 | `sony_cxd6801_demod_atsc3_CheckDemodLock` | `0x0ebd38` | 잠금 상태 레지스터 |
| 8 | `sony_cxd6801_demod_atsc3_CheckALPLock` | `0x0ebe78` | ALP 잠금 레지스터 |

### TODO: 실제 디바이스 테스트

1. HDTV Mate 연결 → `./hdtvmate_detect`로 VID/PID 확인
2. Ghidra에서 추출한 레지스터 값으로 코드 업데이트
3. `./hdtvmate_init`로 하드웨어 초기화 테스트
4. `./hdtvmate_scan`으로 채널 스캔
5. `./hdtvmate_tune`으로 실시간 시청

### TODO: 향후 계획

- [ ] Ghidra 디컴파일 → CXD6801 레지스터 시퀀스 채우기
- [ ] 실제 디바이스 VID/PID 확인 및 반영
- [ ] USB 프로토콜 실제 디바이스 응답에 맞춰 조정
- [ ] libatsc3 core 빌드 (ROUTE/DASH, MMT 프로토콜 스택)
- [ ] macOS SwiftUI 앱 (Phase 6)

## 참조

### 오픈소스

- [libatsc3](https://github.com/kansonkong/libatsc3) — ATSC 3.0 프로토콜 스택 (코어는 공개, PHY는 비공개)
- [Linux kernel af9035.c](https://github.com/torvalds/linux/blob/master/drivers/media/usb/dvb-usb-v2/af9035.c) — IT930x USB 프로토콜 참조
- [Linux kernel cxd2880](https://github.com/torvalds/linux/tree/master/drivers/media/dvb-frontends/cxd2880) — Sony CXD2880 드라이버 (레지스터 패턴 참조)
- [libusb](https://libusb.info/) — 크로스 플랫폼 USB 라이브러리

### 하드웨어 정보

- [HDTV Mate Review (Lon Seidman)](https://blog.lon.tv/2024/01/17/gt-media-hdtv-mate-the-most-affordable-atsc-3-tuner-so-far/)
- [Sony ATSC 3.0 Receiver LSI Paper](https://www.atsc.org/wp-content/uploads/2021/01/f-36-26-13345252_W2dnWCqD_sony_ATSC3.0_receiverLSI_rev1.0.pdf)
- [NGBP.org — ATSC 3.0 Open Source Tools](https://www.ngbp.org/p/atsc-30-ngbp-open-source-tools.html)

## 라이선스

이 프로젝트는 교육 및 연구 목적의 리버스 엔지니어링 결과물입니다.
