# 浏览器刷写 / Browser flashing

小纸 Pico 的浏览器安装页使用 ESP Web Tools 与 Web Serial，通过 HTTPS 连接 ESP32-S3，并写入一个 ESP-IDF 合并固件镜像。

The browser installer uses ESP Web Tools and Web Serial over HTTPS to write one merged ESP-IDF image to the ESP32-S3.

## 发布流程 / Release flow

1. 在准备发布的固件提交上创建并推送版本标签，例如 `v0.1.0`。标签可直接指向 `pico-os` 分支的提交，不要求先合并 PR。
2. `release-firmware.yml` 使用 ESP-IDF v6.1 和 `sdkconfig.defaults` 构建产品固件，再生成合并镜像、`pico-os-app.bin` 与 SHA-256 文件。
3. 工作流将这些镜像附加到 GitHub Release，并把合并镜像与刷写网页一起部署到 GitHub Pages。
4. 首次使用 Pages 时，需在仓库 Settings → Pages 将 Build and deployment source 设为 GitHub Actions。

Create and push a version tag such as `v0.1.0` on the firmware commit you intend to publish. The workflow embeds that tag in the app description, builds with ESP-IDF v6.1 and product `sdkconfig.defaults`, and publishes the merged image plus the OTA app binary and SHA-256 file. For the first deployment, set Settings → Pages → Build and deployment source to GitHub Actions.

## 使用方式 / Install

1. 打开 [小纸 Pico 浏览器刷机页](https://zackerywong233.github.io/read_pico_firmware/)。
2. 使用最新版 Chrome 或 Edge，确保浏览器支持 Web Serial。
3. 用 USB 数据线连接设备，点击“连接并安装”，在系统弹窗里选择 Pico 的串口。
4. 首次升级到双 OTA 分区版本时，勾选擦除 ESP Flash，重建分区表并创建 7 MB 内置存储。擦除会清除 ESP 内置存储和设置；TF 卡上的文件不会被擦除。
5. 等到页面显示完成后再断开 USB 并重启设备。
6. 以后可在设备“设置 → 系统更新”通过 Wi-Fi 安装 GitHub Releases 新版本；也可在传书网页展开“固件升级”上传 `pico-os-app.bin`。
7. OTA 新镜像首次启动会检查板级、VCOM、屏幕电源、字库和触摸初始化，通过后才确认；关键步骤失败会请求 bootloader 回滚到旧槽。

Open the installer in current Chrome or Edge, connect over USB, select the Pico serial port, and wait for the completion message before disconnecting. When moving from the old partition table to dual OTA, select the erase flash option so the new partition table and 7 MB internal storage are created. Erasing clears internal storage and settings; files on the TF card remain untouched. Later releases can be installed from Settings → System Update over Wi-Fi, or by uploading `pico-os-app.bin` in the transfer webpage.

An OTA image is confirmed only after board, VCOM, panel power, font and touch initialization succeeds. A critical boot failure asks the bootloader to return to the previous OTA slot.

## USB 未识别 / USB not detected

若串口未出现，先换 USB-A 数据线；再让设备睡眠后唤醒并重试；最后重启开发板。刷写中断时保持电源稳定，重新连接后重试。

If no serial port appears, try a USB-A data cable, then retry after a sleep/wake cycle, and restart the board if needed. If flashing is interrupted, keep power stable, reconnect, and retry.

## 维护说明 / Maintainer notes

- `sdkconfig.ci` 仅用于编译检查。可发布固件必须从产品配置 `sdkconfig.defaults` 构建。
- ESP Web Tools 需要合并镜像：清单将它写在 Flash 偏移 `0x0`。
- 网页与固件一起部署在 GitHub Pages，清单使用同源相对路径，以满足浏览器跨域读取要求。
- 新版本标签应与仓库发布策略一致；发布前应先完成功能和真机验证。

- `sdkconfig.ci` is compile-only. Release images must use the product `sdkconfig.defaults`.
- ESP Web Tools requires a merged image; the manifest writes it at flash offset `0x0`.
- The page and image are deployed together to GitHub Pages and the manifest uses a same-origin relative URL for browser access.
- Follow the repository release policy and complete feature and device validation before creating a release tag.
