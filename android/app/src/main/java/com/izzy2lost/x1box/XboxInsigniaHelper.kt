package com.izzy2lost.x1box

import java.io.File
import java.io.IOException

internal object XboxInsigniaHelper {
  const val PRIMARY_DNS = "46.101.64.175"
  const val SECONDARY_DNS = "8.8.8.8"

  private const val FLAG_XBOXDASH_XBE = 1 shl 0
  private const val FLAG_XODASH_DIR = 1 shl 1
  private const val FLAG_XBOX_XTF = 1 shl 2
  private const val FLAG_MSDASH_XBE = 1 shl 3
  private const val FLAG_AUDIO_DIR = 1 shl 4
  private const val FLAG_FONTS_DIR = 1 shl 5
  private const val FLAG_XBOXDASHDATA_DIR = 1 shl 6

  data class DashboardStatus(
    val flags: Int,
  ) {
    val hasBootXbe: Boolean
      get() = (flags and FLAG_XBOXDASH_XBE) != 0

    val hasXodashAssets: Boolean
      get() = (flags and FLAG_XODASH_DIR) != 0

    val hasXboxFont: Boolean
      get() = (flags and FLAG_XBOX_XTF) != 0

    val hasMsdashXbe: Boolean
      get() = (flags and FLAG_MSDASH_XBE) != 0

    val hasAudioDir: Boolean
      get() = (flags and FLAG_AUDIO_DIR) != 0

    val hasFontsDir: Boolean
      get() = (flags and FLAG_FONTS_DIR) != 0

    val hasXboxdashdataDir: Boolean
      get() = (flags and FLAG_XBOXDASHDATA_DIR) != 0

    val looksRetailDashboardInstalled: Boolean
      get() = hasBootXbe && (
        hasXodashAssets ||
          hasXboxFont ||
          hasMsdashXbe ||
          hasAudioDir ||
          hasFontsDir ||
          hasXboxdashdataDir
      )

    val hasAnyRetailDashboardFiles: Boolean
      get() = flags != 0
  }

  private val primaryDnsBytes = parseIpv4(PRIMARY_DNS)
  private val secondaryDnsBytes = parseIpv4(SECONDARY_DNS)

  @Throws(IOException::class, IllegalArgumentException::class)
  fun inspectDashboard(hddFile: File): DashboardStatus {
    require(hddFile.isFile) { "No local HDD image is configured." }
    return DashboardStatus(inspectDashboardFlagsPureKotlin(hddFile))
  }

  // Absolute byte offsets of the four DNS pairs inside the Xbox config area
  // at the start of the disk (static / Xbox Live / DHCP / PPPoE blocks).
  // Mirrors the native HDD tool; kept in pure Kotlin because the QEMU block
  // layer must never be initialized inside the process that will (or does)
  // run the emulator — qemu_clock_init aborts on the second initialization.
  private val PRIMARY_DNS_OFFSETS = longArrayOf(0x102C, 0x1060, 0x1178, 0x118C)
  private val SECONDARY_DNS_OFFSETS = longArrayOf(0x1030, 0x1064, 0x117C, 0x1190)
  private const val CONFIG_MINIMUM_BYTES = 0x1194L

  @Throws(IOException::class, IllegalArgumentException::class)
  fun applyConfigSectorDns(hddFile: File) {
    require(hddFile.isFile) { "No local HDD image is configured." }
    java.io.RandomAccessFile(hddFile, "rw").use { raf ->
      require(raf.length() >= CONFIG_MINIMUM_BYTES) {
        "The current HDD image is too small to contain the Xbox config sector"
      }
      val magic = ByteArray(4)
      raf.seek(0)
      raf.readFully(magic)
      val isQcow2 = magic[0] == 'Q'.code.toByte() && magic[1] == 'F'.code.toByte() &&
        magic[2] == 'I'.code.toByte() && magic[3] == 0xFB.toByte()
      require(!isQcow2) {
        "QCOW2 HDD images need the desktop setup flow; in-app setup supports raw hdd.img"
      }
      for (off in PRIMARY_DNS_OFFSETS) {
        raf.seek(off)
        raf.write(primaryDnsBytes)
      }
      for (off in SECONDARY_DNS_OFFSETS) {
        raf.seek(off)
        raf.write(secondaryDnsBytes)
      }
      raf.fd.sync()
    }
  }

  fun primaryDnsBytes(): ByteArray = primaryDnsBytes.copyOf()

  private fun parseIpv4(value: String): ByteArray {
    val octets = value.split('.')
    require(octets.size == 4) { "Invalid IPv4 address: $value" }
    return ByteArray(4) { index ->
      val octet = octets[index].toIntOrNull()
        ?: throw IllegalArgumentException("Invalid IPv4 address: $value")
      require(octet in 0..255) { "Invalid IPv4 address: $value" }
      octet.toByte()
    }
  }

  // ---------------------------------------------------------------------
  // Pure-Kotlin read-only FATX probe of the retail C partition. Mirrors the
  // native xemu_fatx_inspect_retail_dashboard() root scan; kept out of JNI
  // because the native HDD tools initialize the QEMU block layer, and the
  // process that runs (or will run) the emulator aborts in qemu_clock_init
  // on the second initialization. Reads raw hdd.img directly and qcow2
  // through a minimal read-only cluster mapper (no backing file, no
  // encryption, no compressed clusters — all fail closed).
  // ---------------------------------------------------------------------

  private const val PARTITION_C_OFFSET = 0x8CA80000L
  private const val PARTITION_C_SIZE = 0x1F400000L
  private const val FATX_SIGNATURE = 0x58544146L // "FATX" little-endian
  private const val FATX_SUPERBLOCK_SIZE = 4096L
  private const val FATX_SECTOR_SIZE = 512L
  private const val FATX_DIR_ENTRY_SIZE = 64
  private const val FATX_MAX_FILENAME_LEN = 42
  private const val FATX_ATTR_DIRECTORY = 1 shl 4
  private const val FATX_RESERVED_FAT_ENTRIES = 1L
  // Defensive bound on the root directory chain (native trusts the FAT; a
  // corrupt cyclic chain must not hang the settings thread here).
  private const val MAX_ROOT_DIR_CLUSTERS = 65536

  @Throws(IOException::class)
  private fun inspectDashboardFlagsPureKotlin(hddFile: File): Int {
    java.io.RandomAccessFile(hddFile, "r").use { raf ->
      val magic = ByteArray(4)
      raf.seek(0)
      raf.readFully(magic)
      val isQcow2 = magic[0] == 'Q'.code.toByte() && magic[1] == 'F'.code.toByte() &&
        magic[2] == 'I'.code.toByte() && magic[3] == 0xFB.toByte()
      val reader: HddReader = if (isQcow2) Qcow2Reader(raf) else RawReader(raf)

      val superblock = ByteArray(16)
      reader.readFully(PARTITION_C_OFFSET, superblock)
      if (leUInt(superblock, 0) != FATX_SIGNATURE) {
        throw IOException("The C partition is not formatted as FATX")
      }
      val sectorsPerCluster = leUInt(superblock, 8)
      val rootCluster = leUInt(superblock, 12)
      val bytesPerCluster = sectorsPerCluster * FATX_SECTOR_SIZE
      if (bytesPerCluster == 0L || sectorsPerCluster > 1024L) {
        throw IOException("The C partition has an invalid FATX cluster size")
      }

      val fatEntryCount = PARTITION_C_SIZE / bytesPerCluster + FATX_RESERVED_FAT_ENTRIES
      val fat16 = fatEntryCount < 0xFFF0L
      var fatSize = fatEntryCount * (if (fat16) 2L else 4L)
      if (fatSize % FATX_SUPERBLOCK_SIZE != 0L) {
        fatSize += FATX_SUPERBLOCK_SIZE - (fatSize % FATX_SUPERBLOCK_SIZE)
      }
      val fatOffset = PARTITION_C_OFFSET + FATX_SUPERBLOCK_SIZE
      val clusterOffset = fatOffset + fatSize
      if (rootCluster < FATX_RESERVED_FAT_ENTRIES || rootCluster >= fatEntryCount) {
        throw IOException("The C partition has an invalid FATX root cluster")
      }

      var flags = 0
      var cluster = rootCluster
      val entriesPerCluster = (bytesPerCluster / FATX_DIR_ENTRY_SIZE).toInt()
      val entryBuf = ByteArray(FATX_DIR_ENTRY_SIZE)
      var clustersWalked = 0

      while (true) {
        if (++clustersWalked > MAX_ROOT_DIR_CLUSTERS) {
          throw IOException("The FATX root directory chain does not terminate")
        }
        val clusterBase = clusterOffset +
          (cluster - FATX_RESERVED_FAT_ENTRIES) * bytesPerCluster
        for (entryIndex in 0 until entriesPerCluster) {
          reader.readFully(clusterBase + entryIndex.toLong() * FATX_DIR_ENTRY_SIZE, entryBuf)
          val nameLen = entryBuf[0].toInt() and 0xFF
          if (nameLen == 0xFF || nameLen == 0x00) {
            return flags
          }
          if (nameLen == 0xE5) {
            continue
          }
          if (nameLen > FATX_MAX_FILENAME_LEN) {
            throw IOException("Encountered an invalid FATX filename length")
          }
          val attributes = entryBuf[1].toInt() and 0xFF
          val name = String(entryBuf, 2, nameLen, Charsets.US_ASCII)
          flags = flags or classifyRootEntry(name, attributes)
        }

        val fatEntry = readFatEntry(reader, fatOffset, cluster, fat16, fatEntryCount)
        if (!isDataCluster(fatEntry, fat16)) {
          throw IOException("Expected another FATX directory cluster")
        }
        if (fatEntry < FATX_RESERVED_FAT_ENTRIES || fatEntry >= fatEntryCount) {
          throw IOException("Cluster $fatEntry is out of range")
        }
        cluster = fatEntry
      }
    }
  }

  private fun leUInt(buf: ByteArray, offset: Int): Long {
    return (buf[offset].toLong() and 0xFF) or
      ((buf[offset + 1].toLong() and 0xFF) shl 8) or
      ((buf[offset + 2].toLong() and 0xFF) shl 16) or
      ((buf[offset + 3].toLong() and 0xFF) shl 24)
  }

  private fun classifyRootEntry(name: String, attributes: Int): Int {
    val isDir = (attributes and FATX_ATTR_DIRECTORY) != 0
    return if (isDir) {
      when {
        name.equals("xodash", ignoreCase = true) -> FLAG_XODASH_DIR
        name.equals("audio", ignoreCase = true) -> FLAG_AUDIO_DIR
        name.equals("fonts", ignoreCase = true) -> FLAG_FONTS_DIR
        name.startsWith("xboxdashdata.", ignoreCase = true) -> FLAG_XBOXDASHDATA_DIR
        else -> 0
      }
    } else {
      when {
        name.equals("xboxdash.xbe", ignoreCase = true) -> FLAG_XBOXDASH_XBE
        name.equals("msdash.xbe", ignoreCase = true) -> FLAG_MSDASH_XBE
        name.equals("xbox.xtf", ignoreCase = true) -> FLAG_XBOX_XTF
        else -> 0
      }
    }
  }

  @Throws(IOException::class)
  private fun readFatEntry(
    reader: HddReader,
    fatOffset: Long,
    index: Long,
    fat16: Boolean,
    fatEntryCount: Long,
  ): Long {
    if (index >= fatEntryCount) {
      throw IOException("FAT index $index is out of range")
    }
    return if (fat16) {
      val buf = ByteArray(2)
      reader.readFully(fatOffset + index * 2, buf)
      ((buf[0].toLong() and 0xFF) or ((buf[1].toLong() and 0xFF) shl 8))
    } else {
      val buf = ByteArray(4)
      reader.readFully(fatOffset + index * 4, buf)
      leUInt(buf, 0)
    }
  }

  private fun isDataCluster(rawEntry: Long, fat16: Boolean): Boolean {
    // Mirror the native classifier: FAT16 entries are sign-extended from
    // 16 bits before the end/reserved-range comparison, and a raw
    // 0x0000FFFF is an end-of-chain marker on either FAT width.
    val extended = if (fat16) (rawEntry.toShort().toLong() and 0xFFFFFFFFL) else rawEntry
    return extended != 0L && extended != 0xFFFFL && extended < 0xFFFFFFF0L
  }

  private interface HddReader {
    @Throws(IOException::class)
    fun readFully(position: Long, out: ByteArray)
  }

  private class RawReader(private val raf: java.io.RandomAccessFile) : HddReader {
    override fun readFully(position: Long, out: ByteArray) {
      raf.seek(position)
      raf.readFully(out)
    }
  }

  /* Minimal read-only qcow2 (v2/v3) cluster mapper. Enough for the FATX
   * probe's small reads of a healthy xemu HDD image: standard clusters map
   * through L1/L2; unallocated or all-zero clusters read as zeros; backing
   * files, encryption, compressed clusters, and external data files are
   * rejected with a clear error instead of guessing. */
  private class Qcow2Reader(private val raf: java.io.RandomAccessFile) : HddReader {
    private val clusterBits: Int
    private val clusterSize: Long
    private val virtualSize: Long
    private val l1TableOffset: Long
    private val l1Size: Long
    private val l2Entries: Long
    private val incompatibleFeatures: Long

    init {
      val header = ByteArray(104)
      raf.seek(0)
      raf.readFully(header, 0, 72)
      val version = beUInt(header, 4)
      if (version != 2L && version != 3L) {
        throw IOException("Unsupported qcow2 version $version")
      }
      if (version >= 3L) {
        raf.seek(72)
        raf.readFully(header, 72, 32)
        incompatibleFeatures = beULong(header, 72)
        // Bit 0 = dirty (refcounts inconsistent; reads stay valid),
        // bit 2 = external data file (data is elsewhere — reject).
        if ((incompatibleFeatures and (1L shl 2)) != 0L) {
          throw IOException("qcow2 external data files are not supported")
        }
      } else {
        incompatibleFeatures = 0L
      }
      val backingFileOffset = beULong(header, 8)
      if (backingFileOffset != 0L) {
        throw IOException("qcow2 backing files are not supported")
      }
      clusterBits = beUInt(header, 20).toInt()
      if (clusterBits < 9 || clusterBits > 21) {
        throw IOException("Invalid qcow2 cluster size")
      }
      clusterSize = 1L shl clusterBits
      virtualSize = beULong(header, 24)
      if (beUInt(header, 32) != 0L) {
        throw IOException("Encrypted qcow2 images are not supported")
      }
      l1Size = beUInt(header, 36)
      l1TableOffset = beULong(header, 40)
      l2Entries = 1L shl (clusterBits - 3)
    }

    override fun readFully(position: Long, out: ByteArray) {
      var pos = position
      var outOffset = 0
      while (outOffset < out.size) {
        val within = pos and (clusterSize - 1)
        val chunk = minOf(out.size - outOffset, (clusterSize - within).toInt())
        readWithinCluster(pos, out, outOffset, chunk)
        pos += chunk
        outOffset += chunk
      }
    }

    private fun readWithinCluster(pos: Long, out: ByteArray, outOffset: Int, len: Int) {
      if (pos + len > virtualSize) {
        throw IOException("Read beyond the qcow2 virtual disk size")
      }
      val clusterIndex = pos ushr clusterBits
      val l1Index = clusterIndex / l2Entries
      val l2Index = clusterIndex % l2Entries
      if (l1Index >= l1Size) {
        throw IOException("qcow2 L1 index out of range")
      }
      val l1Entry = readBeULongAt(l1TableOffset + l1Index * 8)
      val l2TableOffset = l1Entry and 0x00FFFFFFFFFFFE00L
      if (l2TableOffset == 0L) {
        java.util.Arrays.fill(out, outOffset, outOffset + len, 0)
        return
      }
      val l2Entry = readBeULongAt(l2TableOffset + l2Index * 8)
      if ((l2Entry and (1L shl 62)) != 0L) {
        throw IOException("Compressed qcow2 clusters are not supported")
      }
      // v3 all-zeros flag (bit 0) or an unallocated standard cluster.
      val hostOffset = l2Entry and 0x00FFFFFFFFFFFE00L
      if ((l2Entry and 1L) != 0L || hostOffset == 0L) {
        java.util.Arrays.fill(out, outOffset, outOffset + len, 0)
        return
      }
      raf.seek(hostOffset + (pos and (clusterSize - 1)))
      raf.readFully(out, outOffset, len)
    }

    private fun readBeULongAt(position: Long): Long {
      val buf = ByteArray(8)
      raf.seek(position)
      raf.readFully(buf)
      return beULong(buf, 0)
    }

    private fun beUInt(buf: ByteArray, offset: Int): Long {
      return ((buf[offset].toLong() and 0xFF) shl 24) or
        ((buf[offset + 1].toLong() and 0xFF) shl 16) or
        ((buf[offset + 2].toLong() and 0xFF) shl 8) or
        (buf[offset + 3].toLong() and 0xFF)
    }

    private fun beULong(buf: ByteArray, offset: Int): Long {
      var value = 0L
      for (i in 0 until 8) {
        value = (value shl 8) or (buf[offset + i].toLong() and 0xFF)
      }
      return value
    }
  }
}
