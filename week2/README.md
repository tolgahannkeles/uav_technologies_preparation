# nRF24 Transceiver (E01-ML01DP5 + ESP32)

İki ESP32 kartı arasında, Ebyte **E01-ML01DP5** modülü (nRF24L01P + PA/LNA, 100 mW) ile
çift yönlü kablosuz haberleşme örneği. Seri monitörde bir komut yazıp Enter'a bastığınızda
komut diğer karta gönderilir ve orada ekrana yazdırılır.

- Hedef: ESP32, ESP-IDF **v5.3.5**
- Haberleşme: 2.4 GHz, varsayılan kanal 76 (2476 MHz), 250 kbps
- Paket başına en fazla **32 karakter**
- Otomatik onay (ACK): gönderen taraf mesajın karşıya ulaşıp ulaşmadığını öğrenir

## Gerekenler

- 2 adet ESP32 geliştirme kartı
- 2 adet E01-ML01DP5 modülü + 2.4 GHz SMA anten
- Her modül için VCC–GND arasına 10–100 µF kondansatör (önerilir)

## Bağlantı

| E01-ML01DP5 pini | ESP32 | Açıklama |
|---|---|---|
| 1 GND  | GND    | Toprak |
| 2 VCC  | 3.3V   | **En fazla 3.6 V. 5 V bağlamayın, modül bozulur!** |
| 3 CE   | GPIO22 | Chip Enable (aynı zamanda LNA'yı açar) |
| 4 CSN  | GPIO5  | SPI chip select |
| 5 SCK  | GPIO18 | SPI saat |
| 6 MOSI | GPIO23 | SPI veri (ESP32 → modül) |
| 7 MISO | GPIO19 | SPI veri (modül → ESP32) |
| 8 IRQ  | GPIO21 | Kesme çıkışı (aktif düşük) |

Pinler `menuconfig` üzerinden değiştirilebilir.

> Modül gönderim sırasında ~130 mA çeker. Kondansatör olmadan kart resetlenebilir veya
> paketler kaybolabilir. Anteni takmadan modülü çalıştırmayın.

## Derleme ve yükleme

ESP-IDF ortamını açın (ESP-IDF PowerShell / VS Code ESP-IDF eklentisi), sonra proje klasöründe:

```bash
idf.py set-target esp32      # sadece ilk seferde
idf.py menuconfig            # nRF24 Transceiver -> Node ID
idf.py -p COM3 flash monitor # COM3 yerine kendi portunuz
```

Aynı kodu iki karta yükleyin, **tek fark Node ID**:

- 1. kart: `Node ID = 0`
- 2. kart: `Node ID = 1`

### menuconfig ayarları (`nRF24 Transceiver (E01-ML01DP5)`)

| Ayar | Varsayılan | Açıklama |
|---|---|---|
| Node ID | 0 | Bir kart 0, diğeri 1 olmalı |
| RF channel | 76 | Frekans = 2400 + kanal MHz. İki kartta aynı olmalı. ISM bandında kalmak için ≤ 83 |
| Air data rate | 250 kbps | Düşük hız = daha uzun menzil |
| PA level | 3 | 0..3. Masada yan yana test ederken 0 kullanın |
| Pins | yukarıdaki tablo | |

## Kullanım

Monitörde bir komut yazıp Enter'a basın:

```
I (312) nrf24: ready: ch 76 (2476 MHz), 250 kbps, PA level 3
I (318) trx: node 0 ready. Type a command (max 32 chars) and press Enter.
takeoff
[TX] takeoff -> OK
```

Diğer kartın monitöründe:

```
[RX] takeoff
```

`no ACK` görürseniz mesaj karşıya ulaşmamıştır (karşı kart kapalı, kanal farklı,
menzil dışı vb.). Monitörden çıkmak için `Ctrl+]`.

## Proje yapısı

```
nrf_transceiver/
├── main/
│   ├── main.c              uygulama: uart_task + nrf_task
│   └── Kconfig.projbuild   menuconfig ayarları
└── components/nrf24/       nRF24 sürücüsü (tüm alt seviye işlemler)
    ├── include/nrf24.h     sürücünün API'si
    ├── nrf24.c             SPI, CE/IRQ, mod geçişleri, gönderme/alma
    └── nrf24_regs.h        yazmaç adresleri ve bit tanımları
```

## Nasıl çalışıyor?

```
 klavye ──► uart_task ──(kuyruk)──► nrf_task ──SPI──► nRF24 ~~~ 2.4 GHz ~~~ nRF24 ──► nrf_task ──► ekran
```

- **uart_task**: konsoldan karakterleri okur, Enter'a basılınca satırı bir kuyruğa (queue) koyar.
- **nrf_task**: radyoyu yöneten tek task. Kuyrukta komut varsa gönderir, IRQ pini düşükse
  gelen paketleri okuyup yazdırır.
- Radyo normalde **alma (RX)** modundadır, sadece gönderirken kısa süreliğine **gönderme (TX)**
  moduna geçer.
- **Adresler:** Node 0 `NODE0` adresini dinler ve `NODE1`'e gönderir; Node 1 tam tersini yapar.
- **ACK:** Alıcı her paketi aldığında otomatik olarak kısa bir onay paketi döner. Onay gelmezse
  çip paketi kendisi 15 kereye kadar tekrar gönderir.

Sürücü API'si ([nrf24.h](components/nrf24/include/nrf24.h)):

| Fonksiyon | Ne yapar |
|---|---|
| `nrf24_init(&cfg)` | SPI/GPIO'yu kurar, modülü yapılandırır, modül yoksa hata döner |
| `nrf24_start_listening()` | Alma moduna geçer |
| `nrf24_send(data, len, &retries)` | Paket gönderir, ACK gelirse `ESP_OK` döner |
| `nrf24_read(buf, &pipe)` | Gelen paketi okur, uzunluğunu döner (yoksa 0) |
| `nrf24_irq_pending()` | IRQ pini aktif mi (paket geldi mi) |
| `nrf24_set_channel()`, `nrf24_set_pa_level()` | Çalışırken kanal / güç değiştirir |
| `nrf24_power_down()`, `nrf24_power_up()` | Uyku modu (~1 µA) |

## E01-ML01DP5'e özel notlar

Modül sürüş olarak normal bir nRF24L01P ile aynıdır, ancak kılavuzunda iki önemli not var
ve sürücü bunlara göre yazıldı:

1. **LNA, CE pinine bağlıdır.** CE = 1 iken alıcı yükselteci açıktır. Bu yüzden gönderim
   sırasında CE, sadece 10 µs değil, ACK gelene kadar yüksek tutulur.
2. **Yazmaçlar CE = 0 iken yazılmalıdır.**

## Sorun giderme

| Belirti | Olası sebep |
|---|---|
| `module not responding on SPI` | Kablolama (MISO/MOSI ters, CSN yanlış), modüle 3.3 V gelmiyor |
| Hep `no ACK` | Karşı kart kapalı, iki kartın Node ID'si aynı, kanal / hız farklı |
| Yan yanayken çalışmıyor, uzakta çalışıyor | Alıcı doyuma gidiyor: PA level'ı 0 yapın |
| Ara ara reset / paket kaybı | Besleme yetersiz: kondansatör ekleyin, kısa kablo kullanın |
| `[TX] ... -> timeout` + `packet never left the TX FIFO` | CE sinyali modüle ulaşmıyor: CE kablosunu süreklilik testiyle kontrol edin (jumper kablolar sık bozulur) |
| `[TX] ... -> timeout` + `module was reset` | Gönderim anında besleme düşüyor: VCC–GND arasına kondansatör ekleyin |

## Kaynaklar

- [E01-ML01DP5 kullanım kılavuzu (Ebyte)](https://www.cdebyte.com)
- nRF24L01+ Product Specification v1.0 (Nordic Semiconductor)
