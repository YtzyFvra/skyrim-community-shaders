#!/usr/bin/env python3
"""Merge RTX DLSS Booster / six-pass i18n keys into non-English locale files."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TRANS = ROOT / "package" / "SKSE" / "Plugins" / "CommunityShaders" / "Translations"

# printf tokens that translators must preserve
FMT = re.compile(
    r"%(?:\d+\$)?[-+#0 ]*(?:\d+)?(?:\.\d+)?(?:ll|l|h|hh|z|t|j)?[diuoxXfFeEgGaAcspn%]"
    r"|%ux%u"
)


def tokens(text: str) -> list[str]:
    return FMT.findall(text.replace("%%", ""))


ZH: dict[str, str] = {
    "feature.dlss_booster.badge": "专为 NVIDIA RTX 设计",
    "feature.dlss_booster.debug_host": "为方便调试，重建栈单独成页，避免把超分辨率面板挤乱。请把本页当作调试宿主。",
    "feature.dlss_booster.depends_on_upscaling": "本页依赖「超分辨率」实现：D3D 设备、Streamline 与交换链仍由超分辨率持有，本页并不取代它。",
    "feature.dlss_booster.description": "RTX 重建栈的调试宿主。依赖超分辨率，并不取代它。",
    "feature.dlss_booster.extents": "R %ux%u  ->  G %ux%u  ->  N %ux%u  ->  D %ux%u",
    "feature.dlss_booster.key_feature_1": "独立设置文件，不写入超分辨率 JSON",
    "feature.dlss_booster.key_feature_2": "Origin、单线程与 ARISE 三条管线",
    "feature.dlss_booster.key_feature_3": "六 Pass 重建，每 Pass 可自选后端",
    "feature.dlss_booster.key_feature_4": "开启覆盖后本页生效，超分辨率上重叠选项运行时失效",
    "feature.dlss_booster.name": "RTX DLSS Booster",
    "feature.dlss_booster.needs_upscaling": "请先加载超分辨率。本页只托管设置，运行时仍由超分辨率持有。",
    "feature.dlss_booster.no_rtx": "当前适配器没有 DLSS。在 RTX GPU 上这些重建选项才会生效。",
    "feature.dlss_booster.override_enable": "应用本页（覆盖超分辨率）",
    "feature.dlss_booster.override_enable_tooltip": "开启后，本页 JSON 每帧推送到运行时。超分辨率页上相同旋钮仍会绘制，但不会生效。",
    "feature.dlss_booster.override_explainer": "开启下方选项后应用本页设置。开启期间，超分辨率页上重叠的选项在运行时失效：神经重建、帧生成、Reflex 与重建管线。",
    "feature.dlss_booster.override_off_hint": "覆盖未开启。下方控件仅供预览，不会改动正在运行的游戏。",
    "feature.dlss_booster.override_restart": "切换覆盖可能锁定不同的 Present 路径。重启游戏后生效。",
    "feature.dlss_booster.pipeline": "重建管线",
    "feature.dlss_booster.pipeline_arise_help": "ARISE：与单线程相同的 R/D 合同，六 Pass 尾段异步执行。",
    "feature.dlss_booster.pipeline_help": "Origin 让引擎渲染目标停在显示分辨率 D，并通过视口画出逻辑 R。单线程与 ARISE 按逻辑 R 分配引擎，再呈现物理 D。ARISE 把六 Pass 尾段移出游戏线程，适合上古卷轴这类 CPU 瓶颈。",
    "feature.dlss_booster.pipeline_origin_help": "Origin：引擎 RT 停在 D，视口画出逻辑 R。",
    "feature.dlss_booster.pipeline_single_help": "单线程：引擎按逻辑 R 分配；六 Pass 与 Present 仍在游戏线程。",
    "feature.dlss_booster.rgnd_legend": "R = 场景渲染  |  G = ACT1 引导  |  N = 神经工作分辨率  |  D = 显示 / 呈现",
    "feature.dlss_booster.six_pass": "六 Pass 重建",
    "feature.dlss_booster.six_pass_act1_help": "ACT1 SR / DS：可选 Bypass、DLSS 或 FSR3。选 DLSS 时在本 Pass 选质量档与模型预设。",
    "feature.dlss_booster.six_pass_act2_help": "ACT2 SR：DLSS、FSR3、NIS 或 HQ Lanczos。选 DLSS 时同样在本 Pass 选模型预设。",
    "feature.dlss_booster.six_pass_fusion_help": "Fusion：在最终锐化前混合 ACT1 引导与 NR。",
    "feature.dlss_booster.six_pass_help": "每个 Pass 自选后端。某 Pass 使用 DLSS 时，在该 Pass 上选模型预设——没有单独的超分辨率章节。",
    "feature.dlss_booster.six_pass_nr_help": "NR：在 N 上做神经重建。N 的缩放独立于 G 与 D。",
    "feature.dlss_booster.six_pass_rcas_help": "RCAS：最后一 Pass，显示域锐化。与 ACT1/ACT2 的 DLSS 预设无关。",
    "feature.upscaling.act1_backend_bypass": "旁路",
    "feature.upscaling.act1_backend_dlss": "DLSS",
    "feature.upscaling.act1_backend_fsr3": "FSR3",
    "feature.upscaling.act1_dlss_preset": "ACT1 DLSS 预设",
    "feature.upscaling.act1_ds_filter": "滤镜：HQ Lanczos（只要 G 与 N 不同就会运行）",
    "feature.upscaling.act1_ds_header": "ACT1 DS - 引导 G 到神经 N",
    "feature.upscaling.act1_jitter": "ACT1 抖动提交",
    "feature.upscaling.act1_quality": "ACT1 质量",
    "feature.upscaling.act1_sr_backend": "ACT1 SR 后端",
    "feature.upscaling.act1_sr_backend_fixed": "ACT1 SR 后端：DLSS（ARISE 固定）",
    "feature.upscaling.act1_sr_header": "ACT1 SR - 场景 R 到引导 G",
    "feature.upscaling.act2_backend_dlss": "DLSS",
    "feature.upscaling.act2_backend_fsr3": "FSR3",
    "feature.upscaling.act2_backend_lanczos": "HQ Lanczos",
    "feature.upscaling.act2_backend_nis": "NIS",
    "feature.upscaling.act2_dlss_preset": "ACT2 DLSS 预设",
    "feature.upscaling.act2_fusion": "启用 ACT2 Fusion",
    "feature.upscaling.act2_jitter": "ACT2 抖动提交",
    "feature.upscaling.act2_spatial_filter": "ACT2 空间滤镜：HQ Lanczos",
    "feature.upscaling.act2_sr_backend": "ACT2 SR 后端",
    "feature.upscaling.act2_sr_header": "ACT2 SR",
    "feature.upscaling.diag_act1_ok": "ACT1 DLSS 成功帧：%llu",
    "feature.upscaling.diag_act2_ok": "ACT2 成功帧：NIS %llu | DLSS %llu | FSR3 %llu",
    "feature.upscaling.diag_act_submits": "ACT1 提交：%s | ACT2 提交：%s",
    "feature.upscaling.diag_active_execution": "当前执行：%s",
    "feature.upscaling.diag_actual_jitter": "实际抖动：%.4f, %.4f",
    "feature.upscaling.diag_arise_color": "ARISE 颜色：D3D11 拷贝 %llu | D3D12 提交 %llu | 完成 %llu | 可视 %llu | 失败 %llu",
    "feature.upscaling.diag_arise_queue": "ARISE 队列：%u/%u | 高水位 %u | 饱和连续 %u（最大 %u）",
    "feature.upscaling.diag_arise_stats": "ARISE：平面 %llu | VR %llu | 已消费 %llu | 饱和 %llu (%.3f%%)",
    "feature.upscaling.diag_arise_worker": "ARISE Worker NR：排队 %llu | 成功 %llu | 回退 %llu | 失败 %llu",
    "feature.upscaling.diag_available": "可用",
    "feature.upscaling.diag_bypass_graph": "旁路 Model 3 图",
    "feature.upscaling.diag_bypass_nr": "旁路 NR 输出（SR 对比）",
    "feature.upscaling.diag_dimension": "Model 3 尺寸诊断",
    "feature.upscaling.diag_disable_ui": "禁用 Model 3 UI 合成",
    "feature.upscaling.diag_dlss_provider": "DLSS 提供者：ACT1 成功 %llu / 失败 %llu | ACT2 成功 %llu / 失败 %llu",
    "feature.upscaling.diag_dx12_dlss": "DX12 DLSS 提供者：%s",
    "feature.upscaling.diag_force_sync": "强制同步 Model 3",
    "feature.upscaling.diag_frame_slots": "ARISE 帧槽",
    "feature.upscaling.diag_graph_passes": "图 Pass：ACT1 旁路 %llu | Lanczos %llu | NR %llu | Fusion %llu | RCAS %llu",
    "feature.upscaling.diag_graph_recovery": "图恢复：不支持 %llu | 回退 %llu | 失败 %llu",
    "feature.upscaling.diag_input_isolation": "平面 DLSS 输入隔离",
    "feature.upscaling.diag_input_isolation_help": "把当前视口拷到精确的 RenderRes 输入。需要重启。",
    "feature.upscaling.diag_logical_r": "逻辑 R 执行：%s",
    "feature.upscaling.diag_model3_extents": "Model 3 尺寸：逻辑 R %ux%u | 物理 D %ux%u",
    "feature.upscaling.diag_model3_frames": "Model 3 帧：捕获 %llu | 重建 %llu | 回退 %llu | UI %llu",
    "feature.upscaling.diag_model3_path": "ARISE Model 3：%s%s",
    "feature.upscaling.diag_model3_present": "Model 3 Present：逻辑 %llu | 物理 %llu | 背压 %llu | 失败 %llu | 上次 0x%08X",
    "feature.upscaling.diag_mv_dilated": "膨胀",
    "feature.upscaling.diag_mv_double": "2.0x 运动矢量",
    "feature.upscaling.diag_mv_half": "0.5x 运动矢量",
    "feature.upscaling.diag_mv_inverted": "反转运动矢量",
    "feature.upscaling.diag_mv_raw": "引擎原始运动矢量",
    "feature.upscaling.diag_mv_test": "NR 运动矢量测试",
    "feature.upscaling.diag_mv_zero": "零运动矢量",
    "feature.upscaling.diag_nr_status": "状态：%s | NGX：0x%08X | 评估次数：%llu",
    "feature.upscaling.diag_passthrough": "（运行时不可用；直通）",
    "feature.upscaling.diag_prefilter": "完整 NR 输入预滤波",
    "feature.upscaling.diag_prefilter_50": "50% 往返",
    "feature.upscaling.diag_prefilter_66": "66% 往返",
    "feature.upscaling.diag_prefilter_77": "77% 往返",
    "feature.upscaling.diag_prefilter_off": "关",
    "feature.upscaling.diag_renderdoc": "RenderDoc 兼容：已替换 DLSS/NR；图拓扑保持",
    "feature.upscaling.diag_reset_nr": "每帧重置 NR 历史",
    "feature.upscaling.diag_shadow_scheduler": "ARISE 阴影调度器",
    "feature.upscaling.diag_slots_2": "2 槽",
    "feature.upscaling.diag_slots_3": "3 槽",
    "feature.upscaling.diag_tail_async": "ARISE 异步尾段",
    "feature.upscaling.diag_tail_sync": "单线程 Present 尾段",
    "feature.upscaling.diag_unavailable": "不可用",
    "feature.upscaling.diag_visual_validation": "ARISE 视觉校验",
    "feature.upscaling.diag_worker_nr": "ARISE Feature 18 Worker",
    "feature.upscaling.diagnostics": "诊断",
    "feature.upscaling.dlss_model_preset_tooltip": "选择要使用的 DLSS AI 模型预设。\n默认由 NVIDIA 运行时按超分档位选择。\n当前默认：DLAA / 质量 / 平衡为 K，性能为 M，超级性能为 L。\n显式预设会覆盖所有超分档位。",
    "feature.upscaling.dlss_preset_default": "默认",
    "feature.upscaling.dlss_preset_j": "预设 J",
    "feature.upscaling.dlss_preset_k": "预设 K",
    "feature.upscaling.dlss_preset_l": "预设 L",
    "feature.upscaling.dlss_preset_m": "预设 M",
    "feature.upscaling.dlss_resolution_warning": "警告：请求分辨率 %.0f x %.0f 超过 DLSS 支持的最大分辨率 %d x %d。",
    "feature.upscaling.dlss_will_not_function": "DLSS 无法工作。请降低分辨率或改用其他超分方法。",
    "feature.upscaling.dlssg_frame_multiplier": "DLSS-G 帧倍率",
    "feature.upscaling.dlssg_frame_multiplier_tooltip": "每张实绘帧要显示的总帧数。数值越高，生成帧越多。",
    "feature.upscaling.enable_sharpening": "启用锐化",
    "feature.upscaling.enable_sharpening_tooltip": "对 DLSS 输出做 RCAS 锐化。\n默认关闭；DLSS 本身已经足够锐利。",
    "feature.upscaling.engine_r_preset": "引擎 R 预设（需重启）",
    "feature.upscaling.engine_r_preset_active": "本会话生效：%s（重启后应用所选）",
    "feature.upscaling.engine_r_preset_help": "根据物理显示分辨率分配逻辑场景分辨率。",
    "feature.upscaling.ffx_dll_table_title": "AMD FidelityFX DLL（点击打开文件夹）",
    "feature.upscaling.fg_warn_fidelityfx_missing": "警告：未加载 FidelityFX DLL",
    "feature.upscaling.fg_warn_refresh_rate": "警告：需要高刷新率显示器，或强制启用帧生成",
    "feature.upscaling.fg_warn_windowed": "警告：需要窗口模式",
    "feature.upscaling.final_sharpen": "最终锐化",
    "feature.upscaling.footprint_planned": "足迹积分（规划中 / 不可用）",
    "feature.upscaling.force_enable_frame_generation": "强制启用帧生成",
    "feature.upscaling.force_enable_frame_generation_tooltip": "跳过高刷新率检查，让较低 Hz 的显示器也能跑帧生成。\n适合笔记本和老显示器，但生成帧的余量更少。",
    "feature.upscaling.foveated_active": "已激活：中央凹子矩形超分已启用（菜单中或预检失败时会跳过）。",
    "feature.upscaling.foveated_band_width": "带宽",
    "feature.upscaling.foveated_blend_dither": "抖动",
    "feature.upscaling.foveated_blend_dither_desc": "噪声抖动过渡——大子矩形时比羽化更自然。",
    "feature.upscaling.foveated_blend_feather": "羽化",
    "feature.upscaling.foveated_blend_feather_desc": "边界上 N 像素的 smoothstep 过渡，用来藏接缝。",
    "feature.upscaling.foveated_blend_hard_copy": "硬拷贝",
    "feature.upscaling.foveated_blend_hard_copy_desc": "子矩形边界是硬接缝。开销最低。",
    "feature.upscaling.foveated_blur_radius": "模糊半径",
    "feature.upscaling.foveated_dlss_mode_default": "默认",
    "feature.upscaling.foveated_dlss_mode_default_desc": "分眼隔离：每帧 5 次拷贝、2 次 DLSS 评估。全部预设可用。",
    "feature.upscaling.foveated_dlss_mode_faster": "更快",
    "feature.upscaling.foveated_dlss_mode_faster_desc": "视口偏移：1 次快照、2 次遮罩清除、2 次 DLSS 评估。预设 J/K 不可用。",
    "feature.upscaling.foveated_dlss_mode_fsr_desc": "FSR 不使用此项——仅在超分方法为 DLSS 时生效。",
    "feature.upscaling.foveated_dlss_mode_header": "VR DLSS 模式",
    "feature.upscaling.foveated_dlss_mode_label": "DLSS 模式",
    "feature.upscaling.foveated_dlss_mode_tooltip": "默认 — 最高质量。每只眼睛各自隔离颜色/深度/运动矢量，\n避免 DLSS 跨立体中线采样。每眼每帧 5 次拷贝。\n支持全部 DLSS 预设。适合截图，或「更快」出现边缘伪影时。\n\n更快 — 开销更低。DLSS 用视口偏移直接读帧缓冲，不再分眼隔离。\n每帧 1 次快照 + 2 次遮罩清除。\n立体中心附近可能从邻眼采到 1-2 个像素——运动中通常看不见。\n预设 J 与 K 不兼容，会自动钳到 L。",
    "feature.upscaling.foveated_edge_blend_label": "边缘混合",
    "feature.upscaling.foveated_enable": "启用中央凹超分（区域源）",
    "feature.upscaling.foveated_feather_width": "羽化宽度",
    "feature.upscaling.foveated_noise_amount": "噪声量",
    "feature.upscaling.foveated_overview": "中央凹子矩形超分：只有你选的区域跑完整 DLSS/FSR，周边廉价拉伸。显著降低超分开销，代价是周边锐度。仅 VR。",
    "feature.upscaling.foveated_periphery_aa_label": "周边抗锯齿",
    "feature.upscaling.foveated_periphery_aa_none": "无",
    "feature.upscaling.foveated_periphery_aa_temporal": "时间平滑",
    "feature.upscaling.foveated_periphery_aa_temporal_desc": "把拉伸后的周边与运动重投影历史混合，减轻闪烁。",
    "feature.upscaling.foveated_periphery_header": "周边渲染",
    "feature.upscaling.foveated_periphery_tooltip": "子矩形以外的区域用廉价填充，而不是跑所选超分器。\n这些设置控制廉价填充的观感与是否闪烁。\n\n拉伸方式：子矩形外的像素如何从低分辨率缓冲重建。\n完全不影响已超分的子矩形。\n\n周边抗锯齿：用运动补偿历史混合减轻拉伸区的时间闪烁。\n与已超分的子矩形无关。\n\n边缘混合：超分子矩形边缘如何接到拉伸周边。\n硬拷贝留下硬接缝；羽化/抖动会软化。只影响边界。",
    "feature.upscaling.foveated_screenshot_subrect_note": "截图有自己的子矩形；只有需要像素对齐时才把它们对齐。",
    "feature.upscaling.foveated_shared_panel_note": "质量与锐度在超分辨率主面板上——那里的改动同样作用于中央凹渲染。超分方法为 DLSS 时，DLSS 预设也在那边。",
    "feature.upscaling.foveated_smoothing": "平滑",
    "feature.upscaling.foveated_smoothing_tooltip": "越低 = 更多时间历史（更平滑但可能残影）。越高 = 更跟手。",
    "feature.upscaling.foveated_standing_by": "待机：仅在超分方法为 DLSS 或 FSR 时激活。当前未激活。",
    "feature.upscaling.foveated_standing_by_full_eye": "待机：区域是全眼（无裁剪）——在下方子矩形区域缩小它才能看到节省。",
    "feature.upscaling.foveated_stretch_bilinear": "双线性",
    "feature.upscaling.foveated_stretch_bilinear_desc": "双线性：平滑放大渲染缓冲。偏软但干净。",
    "feature.upscaling.foveated_stretch_gaussian": "高斯模糊",
    "feature.upscaling.foveated_stretch_gaussian_desc": "高斯：把周边再糊一层柔焦。中央凹用法的好默认。",
    "feature.upscaling.foveated_stretch_label": "拉伸",
    "feature.upscaling.foveated_stretch_point": "点采样",
    "feature.upscaling.foveated_stretch_point_desc": "点采样：最便宜，像素感明显。适合量化中央凹节省。",
    "feature.upscaling.foveated_subrect_region_desc": "在下方预览中拖选要完整超分的区域。其余廉价拉伸——能省下可观的超分开销。",
    "feature.upscaling.foveated_subrect_region_header": "子矩形区域",
    "feature.upscaling.foveated_tuning": "中央凹超分调校",
    "feature.upscaling.foveated_visualize_regions": "可视化区域",
    "feature.upscaling.foveated_visualize_regions_tooltip": "诊断：把廉价拉伸的周边染红，让未染色的超分子矩形在游戏里一眼能看出来。\n用来确认超分器实际跑在哪里、廉价拉伸填在哪里。\n无性能影响；运行时可开关，无需重启。即使关闭，拖拽下方区域时也会短暂显示。",
    "feature.upscaling.foveated_vr_only": "仅 VR——平面没有等效的镜片周边质量悬崖可利用。",
    "feature.upscaling.frame_generation": "帧生成",
    "feature.upscaling.frame_generation_desc": "帧生成在实绘帧之间插入生成帧，画面更顺滑",
    "feature.upscaling.frame_generation_dlssg_active": "正在使用 NVIDIA DLSS 帧生成（自动）",
    "feature.upscaling.frame_generation_dlssg_available": "NVIDIA DLSS 帧生成可用。",
    "feature.upscaling.frame_generation_fsr_active": "正在使用 AMD FSR 帧生成（自动）",
    "feature.upscaling.frame_generation_fsr_active_preferred": "正在使用 AMD FSR 帧生成（首选）",
    "feature.upscaling.frame_generation_fsr_available": "AMD FSR 帧生成可用。",
    "feature.upscaling.frame_generation_in_menus": "菜单中启用帧生成",
    "feature.upscaling.frame_generation_in_menus_tooltip_1": "游戏菜单打开时也保持帧生成。",
    "feature.upscaling.frame_generation_in_menus_tooltip_2": "可能更顺滑，但会增加菜单输入延迟。",
    "feature.upscaling.frame_generation_proxy_note": "需要 D3D11 到 D3D12 代理，可能带来兼容性问题",
    "feature.upscaling.frame_generation_tooltip": "在实绘帧之间插入生成帧，画面更顺滑。按适配器与下方偏好使用 NVIDIA\nDLSS-G 或 AMD FSR 帧生成。\n需要 D3D11 到 D3D12 代理交换链，可能引入兼容问题；\n帧生成仅在窗口模式下工作。",
    "feature.upscaling.frame_limit_refresh_rate": "让低刷新率显示器也能使用帧生成。检测到：%.2f Hz",
    "feature.upscaling.frame_limit_vrr": "帧率限制（可变刷新率）",
    "feature.upscaling.fsr4_active_path": "当前路径",
    "feature.upscaling.fsr4_failed_fallback": "本会话运行时 FSR4 失败——回退到 FSR3。",
    "feature.upscaling.fsr4_runtime_enable": "使用运行时 FSR4",
    "feature.upscaling.fsr4_runtime_failed_fallback": "本会话运行时超分 DLL 失败——改用宿主 FSR3 SDK。",
    "feature.upscaling.fsr_frame_gen_fixed_multiplier": "AMD FSR 帧生成：固定 2x",
    "feature.upscaling.fusion_chroma": "Fusion 色度",
    "feature.upscaling.fusion_color": "Fusion 颜色引导",
    "feature.upscaling.fusion_denoise": "Fusion 降噪",
    "feature.upscaling.fusion_denoise_bilateral": "残差双边",
    "feature.upscaling.fusion_denoise_gaussian": "柔和高斯",
    "feature.upscaling.fusion_denoise_luma": "亮度保持",
    "feature.upscaling.fusion_denoise_off": "关",
    "feature.upscaling.fusion_denoise_strength": "Fusion 降噪强度",
    "feature.upscaling.fusion_denoise_strong": "强力清洁",
    "feature.upscaling.fusion_depth": "Fusion 深度引导",
    "feature.upscaling.fusion_dir_act1": "ACT1 引导 NR",
    "feature.upscaling.fusion_dir_nr": "NR 引导 ACT1",
    "feature.upscaling.fusion_direction": "Fusion 方向",
    "feature.upscaling.fusion_edge": "Fusion 边缘保护",
    "feature.upscaling.fusion_enhanced_guidance": "启用增强引导",
    "feature.upscaling.fusion_frequency": "Fusion 频率引导",
    "feature.upscaling.fusion_luma": "Fusion 亮度",
    "feature.upscaling.fusion_preset": "Fusion 预设",
    "feature.upscaling.fusion_preset_clean": "干净",
    "feature.upscaling.fusion_preset_custom": "自定义",
    "feature.upscaling.fusion_preset_detail": "细节",
    "feature.upscaling.fusion_preset_dreamy": "梦幻",
    "feature.upscaling.fusion_preset_nr_led": "NR 主导",
    "feature.upscaling.fusion_preset_soft": "柔和",
    "feature.upscaling.fusion_residual": "Fusion 残差上限",
    "feature.upscaling.fusion_softness": "Fusion 柔和度",
    "feature.upscaling.guidance_scale": "引导缩放（G/D）",
    "feature.upscaling.injection_point": "注入点：PostInject LDR / UI 之前（固定）",
    "feature.upscaling.jitter_actual": "实际摄像机抖动",
    "feature.upscaling.jitter_zero": "零",
    "feature.upscaling.method": "方法",
    "feature.upscaling.method_locked_opencomposite": "OpenComposite 将 %s=true 时锁定为无。",
    "feature.upscaling.method_tooltip": "选择超分后端。",
    "feature.upscaling.neural_n_percent": "神经 N：占显示 D 像素的 %.1f%%",
    "feature.upscaling.neural_rendering": "神经渲染",
    "feature.upscaling.neural_rendering_auto_mask": "自动遮罩",
    "feature.upscaling.neural_rendering_disable_fg": "启用 DLSS 神经渲染前，请先关闭帧生成并重启游戏。",
    "feature.upscaling.neural_rendering_enable": "启用 DLSS 神经渲染",
    "feature.upscaling.neural_rendering_failed": "DLSS 神经渲染失败，本会话已禁用。请查看 CommunityShaders.log。",
    "feature.upscaling.neural_rendering_header": "DLSS 神经渲染",
    "feature.upscaling.neural_rendering_intensity": "强度",
    "feature.upscaling.neural_rendering_local_structure": "局部结构",
    "feature.upscaling.neural_rendering_local_tone": "局部色调",
    "feature.upscaling.neural_rendering_preset": "调校预设",
    "feature.upscaling.neural_rendering_reset_failure": "重置神经渲染失败状态",
    "feature.upscaling.neural_rendering_skin_structure": "皮肤结构",
    "feature.upscaling.neural_rendering_style": "风格",
    "feature.upscaling.neural_rendering_ui_correction": "UI 校正",
    "feature.upscaling.neural_rendering_unavailable": "需要 DLSS。VR 还需要中央凹默认模式以及已激活的 PerfMode。",
    "feature.upscaling.nr_header": "NR",
    "feature.upscaling.nr_preset_balanced": "平衡",
    "feature.upscaling.nr_preset_custom": "自定义",
    "feature.upscaling.nr_preset_fabric": "织物细节",
    "feature.upscaling.nr_preset_natural": "自然",
    "feature.upscaling.nr_preset_strong": "强烈",
    "feature.upscaling.nr_resolution_scale": "NR 分辨率缩放",
    "feature.upscaling.nr_style_0": "风格 0",
    "feature.upscaling.nr_style_1": "风格 1",
    "feature.upscaling.nr_style_2": "风格 2",
    "feature.upscaling.nr_style_3": "风格 3",
    "feature.upscaling.nr_tuning": "NR 调校",
    "feature.upscaling.perfmode_active_note": "按超分分辨率渲染已激活：方法和超分预设的改动要重启游戏才生效。锐度 / 模型预设 / Reflex 仍然即时生效。",
    "feature.upscaling.pipeline_active_arise": "本会话生效：ARISE 异步尾段",
    "feature.upscaling.pipeline_active_origin": "本会话生效：Origin（引擎 RT 在 D）",
    "feature.upscaling.pipeline_active_single_thread": "本会话生效：单线程同步尾段",
    "feature.upscaling.pipeline_arise": "ARISE 异步 PerfMode",
    "feature.upscaling.pipeline_arise_restart": "ARISE：三槽逻辑 Present 尾段。需要重启；不支持 HDR。",
    "feature.upscaling.pipeline_extents": "R %ux%u -> G %ux%u -> N %ux%u -> D %ux%u",
    "feature.upscaling.pipeline_origin": "Origin（引擎 D，视口 R）",
    "feature.upscaling.pipeline_single_thread": "单线程 PerfMode",
    "feature.upscaling.pipeline_single_thread_restart": "同一套逻辑 R 代理与六 Pass 图，跑在 Present 调用线程上。",
    "feature.upscaling.prefer_fsr_frame_gen": "优先使用 AMD FSR 帧生成",
    "feature.upscaling.prefer_fsr_frame_gen_tooltip": "改用 AMD FSR3 帧生成，而不是 NVIDIA DLSS-G。用于 DLSS-G\n初始化成功却不产出插帧的情况。\n需要重启才生效。",
    "feature.upscaling.profile_preview_custom_crop": "保留自定义裁剪",
    "feature.upscaling.profile_preview_format": "中央凹：{} / {} / {} / {} / {} 混合",
    "feature.upscaling.profile_preview_foveation_off": "中央凹关闭（全眼）",
    "feature.upscaling.quality_balanced": "平衡",
    "feature.upscaling.quality_dlaa": "DLAA / 原生",
    "feature.upscaling.quality_performance": "性能",
    "feature.upscaling.quality_quality": "质量",
    "feature.upscaling.quality_ultra_performance": "超级性能",
    "feature.upscaling.rcas_enable": "启用 RCAS",
    "feature.upscaling.rcas_sharpness": "RCAS 锐度",
    "feature.upscaling.render_at_upscale_res": "按超分分辨率渲染引擎",
    "feature.upscaling.render_at_upscale_res_native_noop": "原生抗锯齿（1x）下无效：已按全分辨率渲染；提高超分预设才会生效。",
    "feature.upscaling.render_at_upscale_res_requires": "按超分分辨率渲染需要 DLSS 或 FSR。请把超分方法改过去以激活。",
    "feature.upscaling.render_at_upscale_res_tooltip": "默认开启。引擎管线按超分渲染分辨率分配渲染目标，而不是 HMD 显示分辨率；超分器（DLSS 或 FSR）把输出写到私有 DisplayRes 纹理。能省下可观的显存与带宽，高 HMD 分辨率时尤其明显。\n\n锁定为启动时选中的超分预设：改预设（或本开关）要重启游戏才生效。原生抗锯齿（1.0x）没有渲染分辨率降低，因此不会锁定，预设改动即时生效。\n\n需要 DLSS 或 FSR。锐度 / 模型预设 / Reflex 仍然即时生效。",
    "feature.upscaling.six_pass_order": "执行顺序：ACT1 SR -> ACT1 DS -> NR -> ACT2 SR -> Fusion -> RCAS",
    "feature.upscaling.sl_dll_table_title": "NVIDIA Streamline DLL（点击打开文件夹）",
    "feature.upscaling.srss": "超分辨率超级采样（SRSS）",
    "feature.upscaling.srss_active_session": "本会话生效：%s，模式 %u，预设 %u，G/D %.2fx",
    "feature.upscaling.srss_mode": "SRSS 模式",
    "feature.upscaling.srss_off": "SRSS 关闭：引导 G 等于显示 D",
    "feature.upscaling.srss_off_label": "SRSS 关",
    "feature.upscaling.srss_on": "SRSS 开",
    "feature.upscaling.srss_preset": "SRSS 预设",
    "feature.upscaling.srss_preset_k": "SRSS 预设：预设 K（固定）",
    "feature.upscaling.srss_recommended": "推荐：超级性能 + 预设 M",
    "feature.upscaling.srss_restart": "会改掉 ACT1 引导 G 的资源合同。",
    "feature.upscaling.streamline_log_level_default": "默认",
    "feature.upscaling.streamline_log_level_off": "关",
    "feature.upscaling.streamline_log_level_verbose": "详细",
    "feature.upscaling.streamline_logging_tooltip": "NVIDIA Streamline 后端日志的详细程度。用于调试 DLSS / DLSS-G 问题。",
    "feature.upscaling.upscale_preset": "超分预设",
    "feature.upscaling.upscale_preset_ignored_render_scale": "已设置显式 VR 渲染缩放时，超分预设会被忽略。",
    "feature.upscaling.vr_perf_upscaling_header": "超分与中央凹",
    "feature.upscaling.vr_render_scale": "VR 渲染缩放",
    "feature.upscaling.vr_render_scale_auto": "自动",
    "feature.upscaling.vr_render_scale_tooltip": "按头显分辨率的百分比渲染游戏，再用 DLSS 或 FSR 超分。\n自动跟随超分预设。\n重启游戏后生效。",
}

# Locales other than zh_CN are loaded from booster_i18n_locales.json next to this file
# if present; otherwise only ZH is merged.


def load_extra_locales() -> dict[str, dict[str, str]]:
    extra = Path(__file__).with_name("booster_i18n_locales.json")
    if not extra.exists():
        return {}
    return json.loads(extra.read_text(encoding="utf-8"))


def validate(en: dict[str, str], patch: dict[str, str], locale: str) -> None:
    for key, text in patch.items():
        if key not in en:
            print(f"[{locale}] unknown key {key}", file=sys.stderr)
            continue
        if tokens(en[key]) != tokens(text):
            print(
                f"[{locale}] format mismatch {key}\n  en: {en[key]!r}\n  tr: {text!r}",
                file=sys.stderr,
            )


def merge_locale(path: Path, en: dict[str, str], patch: dict[str, str]) -> int:
    data = json.loads(path.read_text(encoding="utf-8"))
    added = 0
    for key, text in patch.items():
        if key in en and data.get(key) != text:
            if key not in data:
                added += 1
            data[key] = text
    order = [k for k in en if k != "_meta"]
    ordered = {}
    if "_meta" in data:
        ordered["_meta"] = data["_meta"]
        # These files were generated stubs; drop the "do not edit" note now that we localize.
        if isinstance(ordered["_meta"], dict):
            ordered["_meta"].pop("note", None)
            ordered["_meta"].pop("generator", None)
            ordered["_meta"]["auto_generated"] = False
    for key in order:
        if key in data:
            ordered[key] = data[key]
    extras = sorted(k for k in data if k != "_meta" and k not in en)
    for key in extras:
        ordered[key] = data[key]
    path.write_text(json.dumps(ordered, indent=4, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    return added


def main() -> int:
    en = json.loads((TRANS / "en.json").read_text(encoding="utf-8"))
    patches = {"zh_CN": ZH}
    patches.update(load_extra_locales())
    for locale, patch in patches.items():
        validate(en, patch, locale)
        path = TRANS / f"{locale}.json"
        if not path.exists():
            print(f"skip missing {path.name}")
            continue
        added = merge_locale(path, en, patch)
        print(f"{path.name}: wrote {len(patch)} keys ({added} new)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
