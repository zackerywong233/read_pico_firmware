/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：订阅与在线书库入口页，远程目录和订阅配置由局域网传书服务提供。
 * English: Entry page for feeds and online libraries; LAN transfer service owns source configuration.
 *
 * 冻结：页面退出前停止并等待其网络消费者，再交还共享网络服务。
 * Frozen: Stop and join network consumers before returning the shared network service.
 */
#include "app.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include <string.h>
static void render(app_ctx_t*ctx,uint8_t*fb){(void)ctx;ui_clear_page(fb);ui_draw_header(fb,"订阅书库 Feeds","RSS / Atom · OPDS · WebDAV");ui_text_vc(fb,epd_rotated_display_width()/2,UI_CONTENT_TOP+190,UI_PX_BODY,"订阅与书库配置尚未就绪",EPD_DRAW_ALIGN_CENTER,false);ui_draw_menu_handle(fb,false);}
const app_desc_t app_feeds={.title="订阅书库 Feeds",.detail="RSS / OPDS / WebDAV",.render=render};
