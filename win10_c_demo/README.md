# How to compile on Windows

This directory contains a Windows C demo for ONVIF discovery.

## Prerequisites

- **Visual Studio** (with C++ Desktop Development workload)
- OR **MinGW/GCC** installed on Windows

## Option 1: Compile with Visual Studio (Developer Command Prompt)

1. Open `Developer Command Prompt for VS 20xx`.
2. Navigate to this directory.
3. Run the following command:

```cmd
cl onvif_discover_win.c /link ws2_32.lib
```

4. Run the executable:

```cmd
onvif_discover_win.exe
```

## Option 2: Compile with MinGW (GCC)

1. Open your terminal (PowerShell or CMD).
2. Navigate to this directory.
3. Run the following command:

```cmd
gcc onvif_discover_win.c -o onvif_discover_win.exe -lws2_32
```

4. Run the executable:

```cmd
.\onvif_discover_win.exe
```
