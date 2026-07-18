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
    return DashboardStatus(NativeBridge.nativeInspectDashboardFlags(hddFile.absolutePath))
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

  private object NativeBridge {
    init {
      System.loadLibrary("SDL2")
      System.loadLibrary("xemu")
    }

    external fun nativeInspectDashboardFlags(hddPath: String): Int

    external fun nativeApplyConfigSectorDns(
      hddPath: String,
      primaryDns: ByteArray,
      secondaryDns: ByteArray,
    )
  }
}
