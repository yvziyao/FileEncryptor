import React from 'react';
import {
  AbsoluteFill,
  Sequence,
  interpolate,
  spring,
  useCurrentFrame,
  useVideoConfig,
} from 'remotion';

/* ------------------------------------------------------------------ *
 *  主题
 * ------------------------------------------------------------------ */
const C = {
  ink: '#e8eefc',
  sub: '#9fb2d4',
  dim: '#6b7c99',
  accent: '#3b82f6',
  accentDeep: '#1d4ed8',
  panel: '#0d1626',
  border: '#243450',
  ok: '#10b981',
  warn: '#f59e0b',
  danger: '#ef4444',
  // App（浅色主题）复刻用色
  appBg: '#f3f3f3',
  appText: '#1a1a1a',
  appMuted: '#6b7280',
  appField: '#ffffff',
  appBorder: '#d4d4d4',
};

const FONT = '"Segoe UI", "Microsoft YaHei UI", "Microsoft YaHei", system-ui, sans-serif';
const MONO = 'Consolas, "Cascadia Mono", "Courier New", monospace';

/* ------------------------------------------------------------------ *
 *  动画小工具
 * ------------------------------------------------------------------ */
const clamp = { extrapolateLeft: 'clamp', extrapolateRight: 'clamp' } as const;

/** 淡入 + 上移 */
function useFadeUp(delay = 0, dur = 22) {
  const frame = useCurrentFrame();
  const t = interpolate(frame, [delay, delay + dur], [0, 1], clamp);
  return { opacity: t, transform: `translateY(${(1 - t) * 34}px)` };
}

/** 打字机效果 */
function typed(text: string, frame: number, start: number, cps = 18) {
  const n = Math.max(0, Math.floor(((frame - start) / 30) * cps));
  return text.slice(0, Math.min(text.length, n));
}

/** 弹入（spring）：返回数值，由调用方自己拼 transform，避免覆盖已有 transform */
function usePop(delay = 0) {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const s = spring({ frame: frame - delay, fps, config: { damping: 14, mass: 0.6 } });
  return { k: 0.85 + s * 0.15, opacity: Math.min(1, s * 1.4) };
}

/* ------------------------------------------------------------------ *
 *  背景
 * ------------------------------------------------------------------ */
const Backdrop: React.FC<{ children?: React.ReactNode }> = ({ children }) => (
  <AbsoluteFill
    style={{
      background:
        'radial-gradient(1400px 900px at 50% -15%, #16294a 0%, #0a1220 55%, #05080f 100%)',
      fontFamily: FONT,
      color: C.ink,
    }}
  >
    {/* 细网格 */}
    <AbsoluteFill
      style={{
        backgroundImage:
          'linear-gradient(rgba(96,165,250,.055) 1px, transparent 1px), linear-gradient(90deg, rgba(96,165,250,.055) 1px, transparent 1px)',
        backgroundSize: '64px 64px',
        maskImage: 'radial-gradient(1200px 700px at 50% 0%, #000 20%, transparent 78%)',
        WebkitMaskImage: 'radial-gradient(1200px 700px at 50% 0%, #000 20%, transparent 78%)',
      }}
    />
    {children}
  </AbsoluteFill>
);

const Kicker: React.FC<{ children: React.ReactNode; delay?: number }> = ({ children, delay = 0 }) => (
  <div
    style={{
      ...useFadeUp(delay),
      fontSize: 22,
      letterSpacing: '.28em',
      color: '#7fb0ff',
      textTransform: 'uppercase',
      marginBottom: 18,
    }}
  >
    {children}
  </div>
);

const H: React.FC<{ children: React.ReactNode; delay?: number; size?: number }> = ({
  children,
  delay = 6,
  size = 66,
}) => (
  <div style={{ ...useFadeUp(delay), fontSize: size, fontWeight: 800, letterSpacing: '-0.02em' }}>
    {children}
  </div>
);

const P: React.FC<{ children: React.ReactNode; delay?: number; size?: number; width?: number }> = ({
  children,
  delay = 14,
  size = 27,
  width = 1100,
}) => (
  <div
    style={{
      ...useFadeUp(delay),
      fontSize: size,
      lineHeight: 1.65,
      color: C.sub,
      maxWidth: width,
      textAlign: 'center',
    }}
  >
    {children}
  </div>
);

const Logo: React.FC<{ size?: number }> = ({ size = 86 }) => (
  <div style={{ display: 'flex', alignItems: 'center', gap: size * 0.22 }}>
    <div
      style={{
        width: size,
        height: size,
        borderRadius: size * 0.24,
        background: 'linear-gradient(145deg,#3b82f6,#1d4ed8)',
        boxShadow: '0 24px 60px rgba(37,99,235,.45), inset 0 2px 0 rgba(255,255,255,.35)',
        display: 'grid',
        placeItems: 'center',
      }}
    >
      <svg width={size * 0.54} height={size * 0.54} viewBox="0 0 24 24" fill="none"
        stroke="#fff" strokeWidth={2} strokeLinecap="round" strokeLinejoin="round">
        <rect x="4" y="10" width="16" height="11" rx="2.5" />
        <path d="M8 10V7a4 4 0 0 1 8 0v3" />
      </svg>
    </div>
    <div style={{ fontSize: size * 0.46, fontWeight: 800, letterSpacing: '-.02em' }}>
      FileEncryptor
    </div>
  </div>
);

/* ================================================================== *
 *  1. 开场
 * ================================================================== */
const Intro: React.FC = () => {
  const pop = usePop(0);
  return (
    <Backdrop>
      <AbsoluteFill style={{ alignItems: 'center', justifyContent: 'center', gap: 26 }}>
        <div style={{ opacity: pop.opacity, transform: `scale(${pop.k})` }}>
          <Logo size={96} />
        </div>
        <H delay={12} size={92}>你的文件，只有你能打开</H>
        <P delay={26} size={30}>Windows 上的文件加密工具 · 开源 · 单文件 · 全程不联网</P>
      </AbsoluteFill>
    </Backdrop>
  );
};

/* ================================================================== *
 *  2. 痛点
 * ================================================================== */
const Problem: React.FC = () => {
  const items = ['网盘被拖库', '硬盘送修', '设备丢失', '借用别人电脑'];
  const frame = useCurrentFrame();
  return (
    <Backdrop>
      <AbsoluteFill style={{ alignItems: 'center', justifyContent: 'center', gap: 30 }}>
        <Kicker>问题</Kicker>
        <H delay={4}>文件放进网盘，就等于明文裸奔</H>
        <P delay={16}>云盘能看、运维能看、传输链路上的每一跳都能看。</P>
        <div style={{ display: 'flex', gap: 18, marginTop: 10 }}>
          {items.map((t, i) => {
            const o = interpolate(frame, [26 + i * 6, 44 + i * 6], [0, 1], clamp);
            return (
              <div
                key={t}
                style={{
                  opacity: o,
                  transform: `translateY(${(1 - o) * 20}px)`,
                  padding: '16px 28px',
                  borderRadius: 999,
                  background: 'rgba(13,22,38,.85)',
                  border: `1px solid ${C.border}`,
                  fontSize: 23,
                  color: '#c3d2ec',
                }}
              >
                {t}
              </div>
            );
          })}
        </div>
      </AbsoluteFill>
    </Backdrop>
  );
};

/* ================================================================== *
 *  复刻 App 界面（浅色主题）
 * ================================================================== */
const WinFrame: React.FC<{ w: number; h: number; title: string; children: React.ReactNode; style?: React.CSSProperties }> = ({
  w, h, title, children, style,
}) => (
  <div
    style={{
      width: w,
      height: h,
      borderRadius: 14,
      overflow: 'hidden',
      background: C.appBg,
      border: '1px solid #2a3450',
      boxShadow: '0 50px 120px rgba(0,0,0,.6), 0 0 0 1px rgba(255,255,255,.06)',
      ...style,
    }}
  >
    {/* 标题栏 */}
    <div
      style={{
        height: 44,
        background: '#eaeaea',
        borderBottom: '1px solid #d8d8d8',
        display: 'flex',
        alignItems: 'center',
        padding: '0 14px',
        gap: 10,
      }}
    >
      <div style={{ width: 20, height: 20, borderRadius: 5, background: '#f5b301', display: 'grid', placeItems: 'center' }}>
        <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="#7a5600" strokeWidth={2}>
          <rect x="4" y="10" width="16" height="11" rx="2" /><path d="M8 10V7a4 4 0 0 1 8 0v3" />
        </svg>
      </div>
      <div style={{ fontSize: 15, color: '#333', flex: 1 }}>{title}</div>
      <div style={{ display: 'flex', gap: 14, color: '#555', fontSize: 16 }}>
        <span>—</span><span>▢</span><span>✕</span>
      </div>
    </div>
    <div style={{ position: 'relative', height: h - 44 }}>{children}</div>
  </div>
);

const AppButton: React.FC<{ label: string; primary?: boolean; press?: number; w?: number }> = ({
  label, primary = true, press = 0, w = 190,
}) => {
  const p = interpolate(press, [0, 1], [1, 0.96], clamp);
  return (
    <div
      style={{
        width: w,
        height: 64,
        borderRadius: 10,
        display: 'grid',
        placeItems: 'center',
        fontSize: 24,
        fontWeight: primary ? 600 : 400,
        color: primary ? '#fff' : '#1a1a1a',
        background: primary
          ? press > 0.2 ? '#1d4ed8' : 'linear-gradient(180deg,#3b82f6,#2563eb)'
          : '#ffffff',
        border: primary ? 'none' : '1px solid #d4d4d4',
        transform: `scale(${p})`,
        boxShadow: primary ? '0 6px 18px rgba(37,99,235,.35)' : '0 1px 2px rgba(0,0,0,.06)',
      }}
    >
      {label}
    </div>
  );
};

const Cursor: React.FC<{ x: number; y: number; opacity?: number; down?: boolean }> = ({
  x, y, opacity = 1, down = false,
}) => (
  <div
    style={{
      position: 'absolute',
      left: x,
      top: y,
      opacity,
      transform: `scale(${down ? 0.86 : 1})`,
      transformOrigin: 'top left',
      pointerEvents: 'none',
      filter: 'drop-shadow(0 3px 6px rgba(0,0,0,.45))',
    }}
  >
    <svg width="30" height="34" viewBox="0 0 24 28" fill="#fff" stroke="#222" strokeWidth="1.4">
      <path d="M2 2 L2 22 L7.5 17 L11 25 L14.5 23.4 L11 15.5 L18 15.5 Z" />
    </svg>
  </div>
);

/* ================================================================== *
 *  3. 演示 A：拖放文件 -> 密码 -> 进度 -> 完成
 * ================================================================== */
const DemoEncrypt: React.FC = () => {
  const frame = useCurrentFrame();

  // 节奏：拖入(8-38) -> 密码框(45-112) -> 点确定(122) -> 进度(130-195) -> 完成
  const dragT = interpolate(frame, [8, 38], [0, 1], clamp);
  const dropDone = frame > 38;

  const prog = dropDone ? interpolate(frame, [155, 220], [0, 100], clamp) : 0;

  const dlgIn = spring({ frame: frame - 42, fps: 30, config: { damping: 16, mass: 0.7 } });
  const dlgOut = interpolate(frame, [140, 152], [1, 0], clamp);
  const dlgOpacity = Math.min(1, dlgIn) * dlgOut;

  const pwd = typed('Tr0ub4dor&3', frame, 62, 10);

  // 光标轨迹（舞台坐标；密码框/确定按钮的位置按对话框实际布局算出来的）
  const cx = interpolate(
    frame,
    [0, 8, 38, 48, 58, 112, 122, 150],
    [1640, 1580, 900, 900, 900, 900, 1061, 1061],
    clamp
  );
  const cy = interpolate(
    frame,
    [0, 8, 38, 48, 58, 112, 122, 150],
    [90, 120, 520, 512, 505, 505, 688, 688],
    clamp
  );
  const clickAt = (f: number) => (frame >= f && frame <= f + 5 ? 1 : 0);
  const donePop = usePop(228);   // 必须在顶层调用，不能写在条件分支里
  const winPop = usePop(6);

  return (
    <Backdrop>
      <AbsoluteFill style={{ alignItems: 'center', justifyContent: 'center' }}>
        <div style={{ position: 'absolute', top: 64, left: 0, right: 0, textAlign: 'center' }}>
          <Kicker delay={0}>演示 · 图形界面</Kicker>
          <H delay={4} size={52}>把文件拖进来就行</H>
        </div>

        {/* App mockup：整体放大 1.5 倍，坐标仍按 1920x1080 编排 */}
        <div style={{ position: 'absolute', inset: 0, transform: 'scale(1.3)', transformOrigin: 'center center' }}>
        {/* 主窗口 */}
        <div style={{ position: 'absolute', left: '50%', top: '50%', transform: `translate(-50%,-50%) scale(${winPop.k})`, opacity: winPop.opacity }}>
          <WinFrame w={760} h={300} title="文件加密工具">
            <div style={{ padding: '42px 44px', display: 'flex', gap: 36, justifyContent: 'center', alignItems: 'center' }}>
              <AppButton label="加密文件" press={dropDone ? interpolate(frame, [128, 134, 144], [0, 1, 0], clamp) : 0} />
              <AppButton label="解密文件" press={0} />
              <AppButton label="安全删除" primary={false} press={0} />
            </div>
            {/* 拖放高亮 */}
            {!dropDone && (
              <div
                style={{
                  position: 'absolute',
                  inset: 12,
                  borderRadius: 10,
                  border: `2px dashed ${dragT > 0.6 ? C.accent : 'transparent'}`,
                  background: dragT > 0.6 ? 'rgba(59,130,246,.08)' : 'transparent',
                }}
              />
            )}
            {/* 被拖动的文件 */}
            {!dropDone && (
              <div
                style={{
                  position: 'absolute',
                  left: interpolate(frame, [10, 45], [1050, 250], clamp) - 900,
                  top: interpolate(frame, [10, 45], [10, 120], clamp) - 40,
                  opacity: interpolate(frame, [10, 20], [0, 1], clamp),
                  display: 'flex',
                  flexDirection: 'column',
                  alignItems: 'center',
                  gap: 6,
                }}
              >
                <div style={{ width: 62, height: 74, borderRadius: 6, background: '#fff', border: '1px solid #c9c9c9', boxShadow: '0 10px 24px rgba(0,0,0,.18)' }} />
                <div style={{ fontSize: 14, color: '#444' }}>报告.docx</div>
              </div>
            )}
            {/* 进度条 */}
            {prog > 0 && (
              <div style={{ position: 'absolute', left: 44, right: 44, bottom: 32, opacity: interpolate(prog, [0, 2], [0, 1], clamp) }}>
                <div style={{ fontSize: 14, color: '#555', marginBottom: 8 }}>
                  {prog >= 100 ? '完成' : '正在加密：报告.docx'}
                </div>
                <div style={{ height: 12, borderRadius: 6, background: '#e2e2e2', overflow: 'hidden' }}>
                  <div style={{ height: '100%', width: `${prog}%`, background: 'linear-gradient(90deg,#22c55e,#16a34a)' }} />
                </div>
                <div style={{ marginTop: 8, fontSize: 13, color: '#666' }}>{Math.round(prog)}%</div>
              </div>
            )}
          </WinFrame>
        </div>

        {/* 密码对话框 */}
        {dlgOpacity > 0.01 && (
          <div
            style={{
              position: 'absolute',
              left: '50%',
              top: '50%',
              transform: `translate(-50%,-46%) scale(${0.94 + dlgIn * 0.06})`,
              opacity: dlgOpacity,
              width: 620,
              borderRadius: 14,
              background: '#f3f3f3',
              boxShadow: '0 40px 90px rgba(0,0,0,.55)',
              border: '1px solid #d8d8d8',
              overflow: 'hidden',
            }}
          >
            <div style={{ padding: '26px 30px 10px', fontSize: 27, fontWeight: 700, color: '#111' }}>设置密码</div>
            <div style={{ padding: '4px 30px 8px', fontSize: 14, color: C.appMuted, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
              C:\Users\你\Documents\报告.docx
            </div>
            {['密码:', '确认:'].map((lb, i) => (
              <div key={lb} style={{ padding: '8px 30px 0' }}>
                <div style={{ fontSize: 15, color: '#333', marginBottom: 6 }}>{lb}</div>
                <div
                  style={{
                    height: 46,
                    borderRadius: 8,
                    background: '#fff',
                    border: `1.5px solid ${i === 0 && dlgOpacity > 0 ? C.accent : '#d4d4d4'}`,
                    display: 'flex',
                    alignItems: 'center',
                    padding: '0 14px',
                    fontSize: 19,
                    letterSpacing: 3,
                    color: '#111',
                  }}
                >
                  {i === 0 ? '•'.repeat(pwd.length) : '•'.repeat(Math.max(0, pwd.length - 3))}
                </div>
              </div>
            ))}
            <div style={{ padding: '16px 30px 6px', display: 'flex', gap: 22, fontSize: 14, color: '#333' }}>
              <span>☑ 显示密码</span>
              <span>☐ 使用诱饵文件</span>
            </div>
            <div style={{ padding: '18px 30px 24px', display: 'flex', gap: 14, justifyContent: 'flex-end' }}>
              <div style={{ width: 110, height: 44, borderRadius: 8, background: '#2563eb', color: '#fff', display: 'grid', placeItems: 'center', fontSize: 16 }}>确定</div>
              <div style={{ width: 110, height: 44, borderRadius: 8, background: '#fff', border: '1px solid #d4d4d4', display: 'grid', placeItems: 'center', fontSize: 16, color: '#111' }}>取消</div>
            </div>
          </div>
        )}

        {/* 完成提示 */}
        {frame > 228 && (
          <div
            style={{
              position: 'absolute',
              left: '50%',
              top: '50%',
              transform: `translate(-50%,-52%) scale(${donePop.k})`,
              opacity: donePop.opacity,
              width: 660,
              borderRadius: 14,
              background: '#f3f3f3',
              border: '1px solid #d8d8d8',
              boxShadow: '0 40px 90px rgba(0,0,0,.55)',
              padding: '34px 38px',
            }}
          >
            <div style={{ fontSize: 24, fontWeight: 700, color: '#111', marginBottom: 12 }}>✓ 加密完成</div>
            <div style={{ fontSize: 16, color: '#444', lineHeight: 1.6 }}>
              已生成 报告.docx.enc（约 2 倍体积，两个槽位）。
              <br />源文件已按设置安全删除。
            </div>
          </div>
        )}

        <Cursor x={cx} y={cy} down={clickAt(110) === 1 || clickAt(200) === 1} opacity={interpolate(frame, [0, 8], [0, 1], clamp)} />
        </div>
      </AbsoluteFill>
    </Backdrop>
  );
};

/* ================================================================== *
 *  4. 可否认加密
 * ================================================================== */
const DemoDeniable: React.FC = () => {
  const frame = useCurrentFrame();
  const card = (i: number, real: boolean) => {
    const o = interpolate(frame, [10 + i * 12, 34 + i * 12], [0, 1], clamp);
    return (
      <div
        style={{
          opacity: o,
          transform: `translateY(${(1 - o) * 26}px)`,
          width: 470,
          borderRadius: 18,
          padding: '26px 30px',
          background: real ? 'rgba(29,78,216,.16)' : 'rgba(13,22,38,.9)',
          border: `1.5px solid ${real ? '#2a5bd7' : C.border}`,
          boxShadow: real ? '0 24px 60px rgba(37,99,235,.22)' : 'none',
        }}
      >
        <div style={{ fontSize: 17, letterSpacing: '.16em', color: '#8ba3c9', marginBottom: 12 }}>
          {real ? '真密码 →' : '假密码 →'}
        </div>
        <div style={{ fontSize: 27, fontWeight: 700 }}>{real ? '你真正要保护的文件' : '一份无关的诱饵文件'}</div>
      </div>
    );
  };
  return (
    <Backdrop>
      <AbsoluteFill style={{ alignItems: 'center', justifyContent: 'center', gap: 34 }}>
        <Kicker>可否认加密</Kicker>
        <H delay={4}>一个文件，两把钥匙</H>
        <div style={{ display: 'flex', gap: 30, marginTop: 8 }}>
          {card(0, true)}
          {card(1, false)}
        </div>
        <P delay={40} width={1250}>
          两个槽位<strong style={{ color: '#fff' }}>完全对称</strong>，文件本身看不出哪个是真，甚至看不出你用了诱饵。
          <br />被胁迫时，交出另一个密码就行。
        </P>
      </AbsoluteFill>
    </Backdrop>
  );
};

/* ================================================================== *
 *  5. 安全删除演示
 * ================================================================== */
const DemoDelete: React.FC = () => {
  const frame = useCurrentFrame();
  const pass = 3;
  const passIdx = Math.min(pass, Math.floor(interpolate(frame, [60, 150], [0, pass + 0.99], clamp)));
  const pct = interpolate(frame, [60, 150], [0, 100], clamp);
  const done = frame > 155;
  return (
    <Backdrop>
      <AbsoluteFill style={{ alignItems: 'center', justifyContent: 'center', gap: 30 }}>
        <Kicker>演示 · 安全删除</Kicker>
        <H delay={4} size={56}>删掉，就要删干净</H>
        <div style={{ width: 900, marginTop: 16 }}>
          <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: 19, color: C.sub, marginBottom: 12 }}>
            <span>{done ? '已删除' : `第 ${Math.max(1, passIdx)} / ${pass} 次覆写`}</span>
            <span>{Math.round(pct)}%</span>
          </div>
          <div style={{ height: 18, borderRadius: 9, background: '#0d1626', border: `1px solid ${C.border}`, overflow: 'hidden' }}>
            <div
              style={{
                height: '100%',
                width: `${pct}%`,
                background: done
                  ? 'linear-gradient(90deg,#10b981,#059669)'
                  : 'linear-gradient(90deg,#ef4444,#f59e0b)',
              }}
            />
          </div>
          <div style={{ marginTop: 22, display: 'flex', gap: 14 }}>
            {['随机数据覆写', '反码覆写', '刷新到磁盘', '删除文件'].map((s, i) => {
              const on = frame > 40 + i * 30;
              return (
                <div
                  key={s}
                  style={{
                    opacity: on ? 1 : 0.28,
                    padding: '10px 18px',
                    borderRadius: 999,
                    border: `1px solid ${on ? '#2a5bd7' : C.border}`,
                    background: on ? 'rgba(29,78,216,.16)' : 'transparent',
                    fontSize: 17,
                    color: on ? '#dbe7ff' : C.dim,
                  }}
                >
                  {s}
                </div>
              );
            })}
          </div>
        </div>
        <P delay={20} width={1100}>先整盘覆写再删除。不是"标记为可覆盖"，是真的把内容盖掉。</P>
      </AbsoluteFill>
    </Backdrop>
  );
};

/* ================================================================== *
 *  6. 命令行演示
 * ================================================================== */
const DemoCli: React.FC = () => {
  const frame = useCurrentFrame();
  const cliPop = usePop(10);
  const lines = [
    { at: 12, text: '> enc.exe 报告.docx -p Tr0ub4dor&3 -s secure', color: '#dbe7ff' },
    { at: 62, text: 'Encrypting 报告.docx (strength: secure, 450k iterations)', color: '#8ba3c9' },
    { at: 92, text: 'Done. Output: 报告.docx.enc', color: '#34d399' },
    { at: 116, text: '> enc.exe --delete 旧资料.docx -w 3', color: '#dbe7ff' },
    { at: 150, text: 'Securely deleting (overwriting 3 passes) ...', color: '#8ba3c9' },
    { at: 176, text: 'Done: 1 file(s)', color: '#34d399' },
  ];
  return (
    <Backdrop>
      <AbsoluteFill style={{ alignItems: 'center', justifyContent: 'center', gap: 26 }}>
        <Kicker>演示 · 命令行</Kicker>
        <H delay={4} size={54}>脚本和批处理也能用</H>
        <div
          style={{
            transform: `scale(${cliPop.k})`,
            opacity: cliPop.opacity,
            width: 1180,
            borderRadius: 12,
            background: '#080d16',
            border: `1px solid ${C.border}`,
            boxShadow: '0 40px 90px rgba(0,0,0,.6)',
            padding: '24px 28px',
            fontFamily: MONO,
            fontSize: 21,
            lineHeight: 1.85,
            minHeight: 330,
          }}
        >
          {lines.map((l, i) => {
            const t = typed(l.text, frame, l.at, 26);
            if (frame < l.at) return null;
            return (
              <div key={i} style={{ color: l.color, whiteSpace: 'pre-wrap' }}>
                {t}
                {frame < l.at + 40 && i === lines.length - 1 ? <span style={{ opacity: 0.9 }}>▋</span> : null}
              </div>
            );
          })}
        </div>
        <P delay={20} size={24}>与图形界面格式互通：拖拽加密的文件，命令行同样能解。</P>
      </AbsoluteFill>
    </Backdrop>
  );
};

/* ================================================================== *
 *  7. 特性网格
 * ================================================================== */
const Features: React.FC = () => {
  const list = [
    ['AES-256-GCM', '认证加密，篡改必被发现'],
    ['PBKDF2 45 万次', '四档强度可选'],
    ['可否认加密', '双槽对称，看不出来'],
    ['安全删除', '多次覆写后删除'],
    ['零依赖', '密码学用 Windows CNG'],
    ['单文件免安装', '放哪都能跑'],
  ];
  const frame = useCurrentFrame();
  return (
    <Backdrop>
      <AbsoluteFill style={{ alignItems: 'center', justifyContent: 'center', gap: 30 }}>
        <Kicker>核心能力</Kicker>
        <H delay={4} size={56}>该有的都有</H>
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 360px)', gap: 22, marginTop: 12 }}>
          {list.map(([t, d], i) => {
            const o = interpolate(frame, [10 + i * 6, 32 + i * 6], [0, 1], clamp);
            return (
              <div
                key={t}
                style={{
                  opacity: o,
                  transform: `translateY(${(1 - o) * 22}px)`,
                  padding: '24px 26px',
                  borderRadius: 14,
                  background: 'rgba(13,22,38,.85)',
                  border: `1px solid ${C.border}`,
                }}
              >
                <div style={{ fontSize: 25, fontWeight: 700, marginBottom: 8 }}>{t}</div>
                <div style={{ fontSize: 18, color: C.sub }}>{d}</div>
              </div>
            );
          })}
        </div>
      </AbsoluteFill>
    </Backdrop>
  );
};

/* ================================================================== *
 *  8. 结尾
 * ================================================================== */
const Outro: React.FC = () => {
  const outroPop = usePop(0);
  return (
  <Backdrop>
    <AbsoluteFill style={{ alignItems: 'center', justifyContent: 'center', gap: 28 }}>
      <div style={{ opacity: outroPop.opacity, transform: `scale(${outroPop.k})` }}><Logo size={92} /></div>
      <H delay={10} size={64}>现在就试试</H>
      <div style={{ display: 'flex', gap: 18, ...useFadeUp(22) }}>
        <div style={{ padding: '18px 30px', borderRadius: 12, fontSize: 22, fontWeight: 700, color: '#fff', background: 'linear-gradient(145deg,#3b82f6,#1d4ed8)', boxShadow: '0 18px 44px rgba(37,99,235,.45)' }}>
          下载便携版
        </div>
        <div style={{ padding: '18px 30px', borderRadius: 12, fontSize: 22, color: '#cfdcf5', border: `1px solid #2a3a5a` }}>安装版</div>
        <div style={{ padding: '18px 30px', borderRadius: 12, fontSize: 22, color: '#cfdcf5', border: `1px solid #2a3a5a` }}>Microsoft Store</div>
      </div>
      <div style={{ ...useFadeUp(30), fontFamily: MONO, fontSize: 24, color: '#9dc0ff' }}>
        gitee.com/yvziyao/FileEncryptor
      </div>
      <div style={{ ...useFadeUp(38), fontSize: 18, color: C.dim }}>开源 · MIT · 支持 Windows 8 及以上（x64）</div>
    </AbsoluteFill>
  </Backdrop>
  );
};

/* ================================================================== *
 *  时间轴
 * ================================================================== */
const D = {
  intro: 130,
  problem: 150,
  demoEncrypt: 340,
  deniable: 330,
  demoDelete: 240,
  demoCli: 300,
  features: 240,
  outro: 210,
};

export const TOTAL_FRAMES =
  D.intro + D.problem + D.demoEncrypt + D.deniable + D.demoDelete + D.demoCli + D.features + D.outro;

export const Promo: React.FC = () => {
  return (
    <AbsoluteFill style={{ background: '#05080f' }}>
      <Sequence durationInFrames={D.intro}><Intro /></Sequence>
      <Sequence from={D.intro} durationInFrames={D.problem}><Problem /></Sequence>
      <Sequence from={D.intro + D.problem} durationInFrames={D.demoEncrypt}><DemoEncrypt /></Sequence>
      <Sequence from={D.intro + D.problem + D.demoEncrypt} durationInFrames={D.deniable}><DemoDeniable /></Sequence>
      <Sequence from={D.intro + D.problem + D.demoEncrypt + D.deniable} durationInFrames={D.demoDelete}><DemoDelete /></Sequence>
      <Sequence from={D.intro + D.problem + D.demoEncrypt + D.deniable + D.demoDelete} durationInFrames={D.demoCli}><DemoCli /></Sequence>
      <Sequence from={D.intro + D.problem + D.demoEncrypt + D.deniable + D.demoDelete + D.demoCli} durationInFrames={D.features}><Features /></Sequence>
      <Sequence
        from={D.intro + D.problem + D.demoEncrypt + D.deniable + D.demoDelete + D.demoCli + D.features}
        durationInFrames={D.outro}
      >
        <Outro />
      </Sequence>
    </AbsoluteFill>
  );
};
