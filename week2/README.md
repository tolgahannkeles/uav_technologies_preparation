# nRF24 Transceiver (E01-ML01DP5 + ESP32)

İki ESP32 kartı arasında, Ebyte **E01-ML01DP5** modülü (nRF24L01P + PA/LNA, 100 mW) ile
çift yönlü kablosuz haberleşme örneği. Seri monitörde bir komut yazıp Enter'a bastığınızda
komut şifrelenip diğer karta gönderilir, orada çözülüp ekrana yazdırılır.

- Hedef: ESP32, ESP-IDF **v5.3.5**
- Haberleşme: 2.4 GHz, 250 kbps
- Kanal atlama: her mesaj 16 kanallık tablodaki bir sonraki kanaldan gider (2402–2480 MHz)
- Şifreleme: **AES-128-CCM** (şifreleme + doğrulama + tekrar saldırısı koruması)
- Komut başına en fazla **20 karakter**
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
| AES-128 key | `000102…0e0f` | 32 hex karakter. İki kartta **aynı** olmalı. Varsayılan anahtar herkese açık, değiştirin |
| Frequency hopping | açık | İki kartta aynı olmalı. Kapatılırsa aşağıdaki sabit kanal kullanılır |
| RF channel | 76 | Sadece kanal atlama kapalıyken. Frekans = 2400 + kanal MHz. ISM bandında kalmak için ≤ 83 |
| Air data rate | 250 kbps | Düşük hız = daha uzun menzil |
| PA level | 3 | 0..3. Masada yan yana test ederken 0 kullanın |
| Pins | yukarıdaki tablo | |

## Kullanım

Monitörde bir komut yazıp Enter'a basın:

```
I (312) nrf24: ready: ch 2 (2402 MHz), 250 kbps, PA level 3
I (318) link_crypto: AES-128-CCM ready, TX counter starts at 0x00010001
I (318) freq_hop: hopping on, 16 channel(s), starting on ch 2
I (318) trx: node 0 ready. Type a command (max 20 chars) and press Enter.
takeoff
[AIR] 00 01 00 01 5c 1e 9a 47 d2 08 b3 61 f0 2c 7e 99 a4 3d 18
[TX] takeoff -> OK (ch 2)
land
[AIR] 00 01 00 02 8e 70 c4 1b 3a f5 27 d9 0c 61 b8 42
[TX] land -> OK (ch 41)
```

`[AIR]` satırı havaya giden paketin kendisidir: ilk 4 bayt sayaç, sonra şifreli komut,
son 8 bayt doğrulama etiketi. Komut metni havada okunamaz.

Diğer kartın monitöründe:

```
[AIR] 00 01 00 01 5c 1e 9a 47 d2 08 b3 61 f0 2c 7e 99 a4 3d 18
[RX] takeoff (ch 2)

[AIR] 00 01 00 02 8e 70 c4 1b 3a f5 27 d9 0c 61 b8 42
[RX] land (ch 41)
```

`no ACK` görürseniz mesaj karşıya ulaşmamıştır (karşı kart kapalı, kanal farklı,
menzil dışı vb.). Monitörden çıkmak için `Ctrl+]`.

## Proje yapısı

```
nrf_transceiver/
├── main/
│   ├── main.c              uygulama: uart_task + nrf_task
│   └── Kconfig.projbuild   menuconfig ayarları
└── components/
    ├── nrf24/              nRF24 sürücüsü (tüm alt seviye işlemler)
    │   ├── include/nrf24.h     sürücünün API'si
    │   ├── nrf24.c             SPI, CE/IRQ, mod geçişleri, gönderme/alma
    │   └── nrf24_regs.h        yazmaç adresleri ve bit tanımları
    ├── link_crypto/        paket şifreleme (AES-128-CCM, mbedTLS)
    │   ├── include/link_crypto.h
    │   └── link_crypto.c
    └── freq_hop/           kanal atlama (nrf24'ün üstünde)
        ├── include/freq_hop.h
        └── freq_hop.c
```

## Nasıl çalışıyor?

```
 klavye ──► uart_task ──(kuyruk)──► nrf_task ──► şifrele ──► kanal seç ──► nRF24 ~~~ 2.4 GHz ~~~ nRF24 ──► çöz ──► ekran
```

Katmanlar:

```
main          ne gönderilecek, ne yazdırılacak
link_crypto   paketi şifrele / doğrula       (radyodan habersiz)
freq_hop      hangi kanaldan gönderilecek    (içerikten habersiz)
nrf24         32 baytı karşıya ulaştır       (donanım)
```

- **uart_task**: konsoldan karakterleri okur, Enter'a basılınca satırı bir kuyruğa (queue) koyar.
- **nrf_task**: radyoyu yöneten tek task. Kuyrukta komut varsa şifreleyip gönderir, IRQ pini
  düşükse gelen paketleri okur, çözer ve yazdırır.
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

## Kanal atlama (freq_hop)

Tek bir kanalda kalan bir link, o kanaldaki parazitten (WiFi, başka bir nRF24, kasıtlı karıştırma)
tamamen etkilenir. Kanal atlamada mesajlar farklı frekanslara dağıtılır; bir kanal bozuksa
sadece o kanala düşen mesajlar etkilenir.

İki kartta da aynı **atlama tablosu** var (`main.c` → `HOP_TABLE`):

```
2, 41, 17, 66, 29, 74, 9, 53, 35, 80, 22, 60, 13, 47, 70, 26
```

- İki kart da tablonun ilk kanalında başlar.
- Her başarılı mesajdan sonra **iki taraf da bir sonraki kanala geçer**: gönderen ACK'yi
  alınca, alıcı paketi okuyunca. Böylece ortak bir saate gerek kalmaz.
- Taraflar senkronu kaybederse (bir kart resetlendi, bir ACK kayboldu, iki kart aynı anda
  gönderdi) gönderen önce kısa bir rastgele süre bekler, sonra mesajı tablodaki diğer kanallarda
  dener. Karşı tarafı bulduğu kanaldan iki taraf birlikte devam eder:

```
I (5120) freq_hop: no answer on ch 66, searching the other channels...
I (5190) freq_hop: peer found on ch 29, back in sync
[TX] hover -> OK (ch 29)
```

Bu **mesaj bazlı** basit bir atlama yöntemi. Gerçek RC/telemetri linklerinde (FrSky, ExpressLRS,
Bluetooth) kanal, sabit zaman aralıklarıyla değiştirilir ve iki taraf ortak bir zaman çizelgesine
göre senkron tutulur. Atlama dizisi de genellikle eşleştirme (binding) sırasında paylaşılan bir
sırdan türetilir; burada anlaşılır olsun diye sabit bir tablo kullanıldı.

## Şifreleme (link_crypto)

nRF24 paketleri havada şifresiz gider: aynı kanaldaki herhangi bir nRF24 modülü paketleri
dinleyebilir, hatta sahte komut gönderebilir. Bir İHA komut hattında bu kabul edilemez.
Bu yüzden her komut göndermeden önce **AES-128-CCM** ile korunur:

```
+-------------+---------------------+-----------+
| sayaç (4)   | şifreli komut (≤20) | etiket (8)|   = en fazla 32 bayt
+-------------+---------------------+-----------+
```

- **Gizlilik:** Komut, anahtarı bilmeyen biri için okunamaz.
- **Doğrulama (etiket/tag):** Anahtarı bilmeyen biri geçerli paket üretemez. Yolda tek bir bit
  değişse bile alıcı paketi `wrong key or modified packet` diyerek reddeder.
- **Tekrar saldırısı koruması:** Kaydedilip tekrar gönderilen eski bir paket (ör. "takeoff")
  sayacı eski olduğu için `replayed packet` diyerek reddedilir.
- **Sayaç (nonce):** Aynı anahtarla aynı sayaç asla tekrar kullanılmamalıdır. Bu yüzden sayacın
  üst 16 biti, flash'ta (NVS) tutulan ve her açılışta artan bir sayaçtır.

Sınırlamalar (ders örneği olduğu için bilerek basit tutuldu):

- İki kart **aynı anahtarı** paylaşır (pre-shared key). Anahtar `sdkconfig` içinde durur;
  `sdkconfig` git'e eklenmez, ama varsayılan anahtar repoda olduğu için değiştirilmelidir.
- Alıcı yeniden başlatıldığında gördüğü son sayacı unutur; açılıştan sonraki ilk paketi
  kontrol edemez.
- Paketin geldiğini, uzunluğunu ve zamanlamasını gizlemez; yalnızca içeriği korur.

## Sorun giderme

| Belirti | Olası sebep |
|---|---|
| `module not responding on SPI` | Kablolama (MISO/MOSI ters, CSN yanlış), modüle 3.3 V gelmiyor |
| Hep `no ACK` | Karşı kart kapalı, iki kartın Node ID'si aynı, kanal / hız / kanal atlama ayarı farklı |
| Sık sık `searching the other channels...` | Normal değil: tablodaki bazı kanallarda parazit var ya da iki kartın `HOP_TABLE`'ı farklı |
| `[TX] ... -> OK` ama karşıda `rejected: wrong key or modified packet` | İki kartın AES anahtarı farklı |
| Yan yanayken çalışmıyor, uzakta çalışıyor | Alıcı doyuma gidiyor: PA level'ı 0 yapın |
| Ara ara reset / paket kaybı | Besleme yetersiz: kondansatör ekleyin, kısa kablo kullanın |
| `[TX] ... -> timeout` + `packet never left the TX FIFO` | CE sinyali modüle ulaşmıyor: CE kablosunu süreklilik testiyle kontrol edin (jumper kablolar sık bozulur) |
| `[TX] ... -> timeout` + `module was reset` | Gönderim anında besleme düşüyor: VCC–GND arasına kondansatör ekleyin |

## Kaynaklar

- [E01-ML01DP5 kullanım kılavuzu (Ebyte)](https://www.cdebyte.com)
- nRF24L01+ Product Specification v1.0 (Nordic Semiconductor)
