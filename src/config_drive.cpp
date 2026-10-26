#include "config_drive.h"

#include <USBMSC.h>

#include "config.h"
#include "web_page_data.h"

namespace drive {

namespace {
// 说明文档的内容（UTF-8，开头带 BOM，访达 / 文本编辑能正确识别中文）
const char README[] =
    "\xEF\xBB\xBF"
    "重要文件，请先阅读\n"
    "\n"
    "本设备没有配套 App，Settings.html 是进入设置的唯一入口，请妥善保管。\n"
    "\n"
    "推荐用法\n"
    "1. 把 Settings.html 复制到你常用的文件夹，例如“文稿”“影片”或“音乐”。\n"
    "2. 用 Chrome 浏览器打开它，并添加为书签，以后点书签即可进入设置。\n"
    "   注意：复制好之后不要再移动或重命名这个文件，否则书签会失效。\n"
    "\n"
    "文件丢失，或不想在电脑里保留时\n"
    "用数据线把 " PRODUCT_NAME " 连接到电脑，用“设置入口指纹”长按 5 秒，本磁盘会再次出现，"
    "从这里打开 Settings.html 即可。\n"
    "\n"
    "升级固件后\n"
    "本磁盘里的 Settings.html 会随固件一起更新；复制到电脑里的那份不会自动更新，"
    "请重新复制一份，覆盖旧文件。\n";
constexpr uint32_t README_LEN = sizeof(README) - 1;

// 极简 FAT12 只读卷，按需现算每个扇区，不占额外闪存：
//   扇区 0：引导扇区   1：FAT1   2：FAT2   3：根目录   4 起：文件内容（一簇 = 一个扇区）
//   Settings.html 从第 2 簇开始，说明文档紧接在后面
constexpr uint16_t SECTOR = 512;
constexpr uint32_t FAT1 = 1, FAT2 = 2, ROOT = 3, DATA = 4;
constexpr uint32_t clustersFor(uint32_t len) { return (len + SECTOR - 1) / SECTOR; }
constexpr uint32_t PAGE_CLUSTERS = clustersFor(WEB_PAGE_LEN);
constexpr uint32_t README_CLUSTERS = clustersFor(README_LEN);
constexpr uint32_t PAGE_FIRST = 2, README_FIRST = PAGE_FIRST + PAGE_CLUSTERS;
constexpr uint32_t USED_CLUSTERS = PAGE_CLUSTERS + README_CLUSTERS;
constexpr uint32_t TOTAL_SECTORS = DATA + USED_CLUSTERS + 16;  // 留一点空闲，部分系统不喜欢“满盘”
static_assert(USED_CLUSTERS + 2 < 340, "文件太大：单扇区 FAT12 最多约 170KB");

const char LABEL[12] = DRIVE_LABEL;  // 11 个字符，不足补空格

struct File {
  const char SHORT_NAME[11];  // 8.3 短文件名
  const char16_t* longName;   // 长文件名（最多 13 个字符 = 1 个目录项）
  uint32_t firstCluster;
  uint32_t length;
};
const File FILES[] = {
    {{'S', 'E', 'T', 'T', 'I', 'N', '~', '1', 'H', 'T', 'M'}, u"Settings.html", PAGE_FIRST, WEB_PAGE_LEN},
    {{'R', 'E', 'A', 'D', 'M', 'E', '~', '1', 'T', 'X', 'T'}, u"重要文件请先阅读.txt", README_FIRST, README_LEN},
};

USBMSC msc;
volatile bool visible = false;

void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF; }

void bootSector(uint8_t* s) {
  const uint8_t jump[3] = {0xEB, 0x3C, 0x90};
  memcpy(s, jump, 3);
  memcpy(s + 3, "MSDOS5.0", 8);
  put16(s + 11, SECTOR);      // 每扇区字节数
  s[13] = 1;                  // 每簇扇区数
  put16(s + 14, 1);           // 保留扇区数
  s[16] = 2;                  // FAT 份数
  put16(s + 17, 16);          // 根目录项数（1 个扇区）
  put16(s + 19, TOTAL_SECTORS);
  s[21] = 0xF8;               // 介质类型
  put16(s + 22, 1);           // 每份 FAT 的扇区数
  put16(s + 24, 1);           // 每磁道扇区数
  put16(s + 26, 1);           // 磁头数
  s[36] = 0x80;               // 驱动器号
  s[38] = 0x29;               // 扩展引导标记
  put32(s + 39, 0x46545950);  // 卷序列号 "FTYP"
  memcpy(s + 43, LABEL, 11);
  memcpy(s + 54, "FAT12   ", 8);
  s[510] = 0x55;
  s[511] = 0xAA;
}

void setFat12(uint8_t* fat, uint32_t cluster, uint16_t value) {
  const uint32_t off = cluster * 3 / 2;
  if (cluster & 1) {
    fat[off] = (fat[off] & 0x0F) | ((value << 4) & 0xF0);
    fat[off + 1] = value >> 4;
  } else {
    fat[off] = value & 0xFF;
    fat[off + 1] = (fat[off + 1] & 0xF0) | ((value >> 8) & 0x0F);
  }
}

void fatSector(uint8_t* s) {
  setFat12(s, 0, 0xFF8);
  setFat12(s, 1, 0xFFF);
  for (const File& f : FILES) {
    const uint32_t n = clustersFor(f.length);
    for (uint32_t c = 0; c < n; c++) {
      const uint32_t cluster = f.firstCluster + c;
      setFat12(s, cluster, c + 1 < n ? cluster + 1 : 0xFFF);
    }
  }
}

uint8_t shortNameChecksum(const char* name) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; i++) sum = ((sum & 1) << 7) + (sum >> 1) + uint8_t(name[i]);
  return sum;
}

// 写一个文件的两个目录项：长文件名（UTF-16）+ 8.3 短文件名
void putEntries(uint8_t* l, const File& f) {
  l[0] = 0x41;  // 序号 1，且是最后一项
  static const uint8_t slots[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  bool ended = false;
  for (int i = 0; i < 13; i++) {
    uint16_t ch;
    if (ended) ch = 0xFFFF;                    // 名字结束后的位置填 0xFFFF
    else if (!f.longName[i]) { ch = 0; ended = true; }  // 名字后面先补一个 0
    else ch = f.longName[i];
    put16(l + slots[i], ch);
  }
  l[11] = 0x0F;
  l[13] = shortNameChecksum(f.SHORT_NAME);

  uint8_t* e = l + 32;
  memcpy(e, f.SHORT_NAME, 11);
  e[11] = 0x21;                              // 只读 + 归档
  const uint16_t date = ((2026 - 1980) << 9) | (10 << 5) | 26;  // 2026-10-26
  put16(e + 16, date);                       // 创建日期
  put16(e + 18, date);                       // 访问日期
  put16(e + 24, date);                       // 修改日期
  put16(e + 26, f.firstCluster);             // 起始簇
  put32(e + 28, f.length);
}

void rootSector(uint8_t* s) {
  // 目录项 0：卷标；之后每个文件占两项
  memcpy(s, LABEL, 11);
  s[11] = 0x08;
  uint8_t* p = s + 32;
  for (const File& f : FILES) {
    putEntries(p, f);
    p += 64;
  }
}

void readSector(uint32_t lba, uint8_t* s) {
  memset(s, 0, SECTOR);
  if (lba == 0) bootSector(s);
  else if (lba == FAT1 || lba == FAT2) fatSector(s);
  else if (lba == ROOT) rootSector(s);
  else if (lba >= DATA) {
    const uint32_t cluster = lba - DATA + 2;
    const File* hit = nullptr;
    for (const File& f : FILES) {
      if (cluster >= f.firstCluster && cluster < f.firstCluster + clustersFor(f.length)) hit = &f;
    }
    if (!hit) return;
    const uint8_t* data = hit == &FILES[0] ? WEB_PAGE : reinterpret_cast<const uint8_t*>(README);
    const uint32_t off = (cluster - hit->firstCluster) * SECTOR;
    memcpy(s, data + off, min<uint32_t>(SECTOR, hit->length - off));
  }
}

int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  uint8_t sector[SECTOR];
  uint8_t* out = static_cast<uint8_t*>(buffer);
  uint32_t done = 0;
  while (done < bufsize) {
    const uint32_t pos = offset + done;
    readSector(lba + pos / SECTOR, sector);
    const uint32_t inSector = pos % SECTOR;
    const uint32_t n = min<uint32_t>(SECTOR - inSector, bufsize - done);
    memcpy(out + done, sector + inSector, n);
    done += n;
  }
  return bufsize;
}

int32_t onWrite(uint32_t, uint32_t, uint8_t*, uint32_t) { return -1; }  // 只读

bool onStartStop(uint8_t, bool start, bool loadEject) {
  if (loadEject && !start) {  // 在访达里点了“推出”
    visible = false;
    msc.mediaPresent(false);
  }
  return true;
}
}  // namespace

void begin() {
  msc.vendorID("Fingertp");
  msc.productID("Settings Disk");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.isWritable(false);
  msc.mediaPresent(false);  // 默认不出现
  msc.begin(TOTAL_SECTORS, SECTOR);
}

void show() {
  visible = true;
  msc.mediaPresent(true);
}

void hide() {
  visible = false;
  msc.mediaPresent(false);
}

bool shown() { return visible; }

}  // namespace drive
