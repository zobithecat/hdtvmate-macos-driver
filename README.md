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
| USB 브리지 | ITE IT9300 (FW v2.65.131.0) | USB ↔ I2C 변환, TS 멀티플렉싱, PID 필터링 |
| 디모듈레이터 | Sony CXD6801 (Chip ID: 0x0396) | ATSC 3.0/1.0/J.83B 복조, ALP/TS 출력 |
| RF 튜너 | Sony ASCOT3 (CXD6801 내장) | RF 주파수 선택, PLL 합성, LNA |

### 확인된 USB/I2C 파라미터

| 파라미터 | 값 | 비고 |
|---------|-----|------|
| USB VID:PID | `23E2:2B02` | Zenview (제조사) |
| USB Endpoints | TX=0x02, RX=0x81, RX_TS=0x84 | Bulk transfer |
| I2C Bus | 3 | IT9300 I2C bus index |
| CXD6801 Write Addr | 0xC8 | Bank select + register write |
| CXD6801 Read Addr | 0xDC | Register pointer + data read |
| I2C CMD Write | 0x002B | Endeavour protocol |
| I2C CMD Read | 0x002A | Endeavour protocol |

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
| `libusb_android_sony.so` | 270 KB | not stripped | Android libusb 래퍼 |

### Ghidra 디컴파일 완료 함수

| 함수 | 주소 | 크기 | 상태 |
|------|------|------|------|
| `X_tune` (ASCOT3 core tune) | 0xdf520 | 4068B | ✅ 완료 → C 구현됨 |
| `X_oscen` (VCO enable) | 0xdf344 | 476B | ✅ 완료 → C 구현됨 |
| `sony_cxd6801_ascot3_Tune` | 0xdf178 | 3500B | ✅ 완료 |
| `sony_cxd6801_ascot3_Create` | 0xde304 | 292B | ✅ 완료 |
| `sony_cxd6801_demod_atsc3_Tune` | 0xea914 | 488B | ✅ 완료 |
| `SLtoAA3` (Sleep→Active ATSC3) | 0xeaafc | 2316B | ✅ 완료 → C 구현됨 |
| `SLtoAA3_BandSetting` | 0xed7f0 | 2336B | ✅ 완료 → C 구현됨 |
| `sony_cxd6801_demod_atsc3_CheckDemodLock` | 0xebd38 | 320B | ✅ 완료 |
| `sony_cxd6801_demod_atsc3_monitor_SyncStat` | 0xef3cc | 588B | ✅ 완료 |
| `sony_cxd6801_demod_TuneEnd` | 0xe3924 | 224B | ✅ 완료 |
| `sony_cxd6801_demod_SoftReset` | 0xe3a04 | 288B | ✅ 완료 |
| `sony_cxd6801_demod_SetStreamOutput` | 0xe3b24 | 596B | ✅ 완료 |
| `sony_cxd6801_demod_I2cRepeaterEnable` | 0xe6e6c | 164B | ✅ 완료 |
| `sony_cxd6801_demod_SetALPClockModeAndFreq` | 0xe8820 | 956B | ✅ 완료 |
| `sony_cxd6801_integ_atsc3_Tune` | 0x105d90 | 760B | ✅ 완료 |

### g_param_table (TV System 파라미터)

바이너리 offset `0x3C4CC`에서 추출. 32 entries × 16 bytes.
주파수 대역별 gain/filter/AGC 설정을 포함.

## macOS 드라이버 (`hdtvmate-macos/`)

### 빌드

```bash
brew install libusb cmake
cd hdtvmate-macos
mkdir build && cd build
cmake ..
make
```

### CLI 도구

```bash
# 디바이스 탐지
./hdtvmate_detect

# 하드웨어 초기화 테스트
./hdtvmate_init

# 튜닝 테스트 (lock 확인만, 캡처 없음)
./hdtvmate_tune_test 701000          # 701 MHz (한국 UHF ch52)
./hdtvmate_tune_test --channel 14    # 473 MHz (US ch14)
./hdtvmate_tune_test 701000 --atsc1  # ATSC 1.0 모드

# 채널 스캔
./hdtvmate_scan

# 튜닝 + TS 캡처
./hdtvmate_tune 701000 -o output.ts
./hdtvmate_tune 701000 --udp 127.0.0.1:1234
vlc udp://@:1234
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
│   │   ├── br_cmd.c            # Endeavour 커맨드 프로토콜
│   │   ├── it9300.c            # IT9300 초기화, GPIO reset, 전원 제어
│   │   └── br_firmware.h       # 추출된 IT9300 펌웨어 (9,385 bytes)
│   ├── demod/
│   │   ├── cxd6801_i2c_ite.c   # I2C over IT9300 (bank select + R/W)
│   │   ├── cxd6801.c           # 디모듈레이터 코어 (init, sleep, acquire)
│   │   ├── cxd6801_atsc3.c     # ATSC 3.0 SLtoAA3 + BandSetting + 잠금
│   │   ├── cxd6801_monitor.c   # SNR/BER/L1/Bootstrap 모니터링
│   │   ├── ascot3.c            # ASCOT3 RF 튜너 (X_oscen + X_tune)
│   │   └── ascot3_tune.h       # X_tune 레지스터 시퀀스 문서
│   ├── scan/
│   │   ├── frequency_table.c   # ATSC 주파수 테이블
│   │   └── channel_scan.c      # 전체/단일 채널 스캔
│   ├── capture/
│   │   ├── circular_buffer.c   # 링 버퍼 (8 MB, 쓰레드 세이프)
│   │   └── capture_thread.c    # USB bulk 캡처 + TLV 처리
│   ├── output/
│   │   ├── udp_output.c        # UDP 멀티캐스트 출력
│   │   └── file_output.c       # TS 파일 저장
│   └── tools/
│       ├── detect.c            # hdtvmate_detect
│       ├── init_test.c         # hdtvmate_init
│       ├── scan.c              # hdtvmate_scan
│       ├── tune.c              # hdtvmate_tune (full capture)
│       ├── tune_test.c         # hdtvmate_tune_test (lock check only)
│       └── diag_test.c         # hdtvmate_diag (register dump)
└── tools/
    ├── extract_firmware.py     # 바이너리에서 IT9300 펌웨어 추출
    └── analyze_symbols.py      # .so 심볼 분석/분류
```

## 현재 상태

### 동작 확인됨 ✅

- [x] USB 디바이스 탐지 및 열기 (VID=23E2, PID=2B02)
- [x] IT9300 브리지 초기화 (GPIO reset, 전원 enable, FW v2.65.131.0)
- [x] CXD6801 Chip ID 읽기 (0x0396)
- [x] I2C 프로토콜 확정 (UTM VM 테스트로 검증)
  - Bank select: CMD 0x2B `[2, bus=3, 0xC8, 0x00, bank]`
  - Data write: CMD 0x2B `[len+1, bus=3, 0xDC, reg, data...]`
  - Reg pointer: CMD 0x2B `[1, bus=3, 0xDC, reg]`
  - Data read: CMD 0x2A `[len, bus=3, 0xDC]`
- [x] ASCOT3 튜너 설정 (X_oscen + X_tune 전체 6단계 레지스터 시퀀스)
- [x] SLtoAA3 모드 전환 (Sleep → Active ATSC 3.0, 15+ register writes)
- [x] BandSetting 6MHz (nominalRate, ITB coefficients, filter)
- [x] SoftReset (acquisition trigger)
- [x] **syncStat=1 달성** (OFDM bootstrap 감지, 한국 UHF 698~767 MHz)
- [x] **안테나 문제 확정** — LineageOS VM에서 Android 앱도 동일하게 lock 실패
- [x] LineageOS UTM VM 셋업 + HDTV Player 앱 동작 확인

### I2C 주소 맵 (확정)

| 주소 | 역할 | 비고 |
|------|------|------|
| 0xC8 | CXD6801 SLV-T write | Bank select + register write (write-only) |
| 0xDC | CXD6801 SLV-T/X read | Reg pointer + data read + data write |
| 0xC0 | ASCOT3 tuner | Android 앱 로그에서 확인 (`I2C Write error addr c0`) |

### 진행중 🔧

- [ ] **I2C write bank select** — bank 0 이외에 write 시 bank 적용 안 됨
  - Bank select(0xC8) 후 write(0xDC)하면 bank 0에만 써짐
  - Sony 드라이버는 `IT9300_writeGenericRegisters`로 0xC8에 직접 write
  - 0xF424 (IT9300 no-stop mode)는 이 FW에서 bus stuck 유발
  - 해결 방안: bank+reg+data를 하나의 I2C 메시지로 합치기
- [ ] syncStat > 5 (full demod lock) 달성
- [ ] 안테나 업그레이드 필요 (실외/증폭기)

### TODO 📋

- [ ] Write bank select 해결 → bank 0x90+ register write 가능하게
- [ ] 제대로 된 UHF 안테나로 demod lock 테스트
- [ ] TS 캡처 → 파일 저장 테스트
- [ ] UDP 출력 → VLC 재생 확인
- [ ] 한국 지상파 채널 스캔 (473~605 MHz, ATSC 3.0 + 1.0)
- [ ] macOS SwiftUI 앱
- [ ] LineageOS VM에서 lock 성공 시 full logcat 분석

## 한국 지상파 UHD 방송 정보

대한민국은 **세계 최초** ATSC 3.0 지상파 UHD 방송을 2017년부터 시행.

| 항목 | 스펙 |
|------|------|
| 전송 방식 | ATSC 3.0 (IP 기반) |
| 주파수 대역 | UHF 470~806 MHz |
| 채널 대역폭 | **6 MHz** |
| 변조 방식 | OFDM |
| 영상 코덱 | HEVC (H.265) |
| 해상도 | 3840×2160 (4K UHD) |
| 수도권 주파수 | 698~710 MHz, 753~771 MHz |

## 참조

### 오픈소스

- [libatsc3](https://github.com/kansonkong/libatsc3) — ATSC 3.0 프로토콜 스택
- [Linux kernel af9035.c](https://github.com/torvalds/linux/blob/master/drivers/media/usb/dvb-usb-v2/af9035.c) — IT930x USB 프로토콜 참조
- [Linux kernel cxd2880](https://github.com/torvalds/linux/tree/master/drivers/media/dvb-frontends/cxd2880) — Sony CXD2880 드라이버 (레지스터 패턴 참조)
- [libusb](https://libusb.info/) — 크로스 플랫폼 USB 라이브러리

### 하드웨어 정보

- [HDTV Mate Review (Lon Seidman)](https://blog.lon.tv/2024/01/17/gt-media-hdtv-mate-the-most-affordable-atsc-3-tuner-so-far/)
- [Sony ATSC 3.0 Receiver LSI Paper](https://www.atsc.org/wp-content/uploads/2021/01/f-36-26-13345252_W2dnWCqD_sony_ATSC3.0_receiverLSI_rev1.0.pdf)

## 라이선스

이 프로젝트는 교육 및 연구 목적의 리버스 엔지니어링 결과물입니다.
