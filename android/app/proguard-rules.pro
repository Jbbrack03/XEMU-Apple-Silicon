# XrMenuBridge is instantiated and invoked only via reflection/JNI from the
# native XR shell (xr_shell_main.c), so R8 can't see those references. Release
# builds currently ship with minify off, but keep it explicitly so enabling R8
# later never strips or renames it.
-keep class com.izzy2lost.x1box.XrMenuBridge { *; }
