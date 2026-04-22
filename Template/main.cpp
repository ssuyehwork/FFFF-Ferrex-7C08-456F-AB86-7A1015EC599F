#include <QApplication>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QButtonGroup>
#include <QTimer>
#include <QDateTime>
#include <QShortcut>
#include <QClipboard>
#include <QMouseEvent>
#include <QGraphicsDropShadowEffect>
#include <QPainter>
#include <QPainterPath>
#include <QVector>
#include <QSettings>
#include <QDebug>
#include <QLineEdit>
#include <QIcon>
#include <QKeyEvent>
#include <QSvgRenderer>
#include <QProcess>
#include <QRegularExpression>
#include <QUuid>
#include <QToolTip>
#include <QScrollArea>
#include <QCheckBox>
#include <QAbstractButton>
#include <QAbstractSlider>
#include <QMenu>
#include <QAction>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#include <winioctl.h>
#endif

// ============================================================================
// KeyboardHook: 全局按键拦截 (支持组合键)
// ============================================================================
struct HotkeyConfig {
    int vk = 0;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    QString name = "F1";

    bool operator==(const HotkeyConfig& other) const {
        return vk == other.vk && ctrl == other.ctrl && alt == other.alt && shift == other.shift;
    }
};
Q_DECLARE_METATYPE(HotkeyConfig)

struct MacroConfig {
    QString id;
    QString name;
    QVector<int> keys;
    bool active = false;

    static MacroConfig createNew(const QVector<int>& keys) {
        MacroConfig cfg;
        cfg.id = QUuid::createUuid().toString();
        cfg.name = QString("宏_%1").arg(QDateTime::currentDateTime().toString("mmss"));
        cfg.keys = keys;
        cfg.active = false;
        return cfg;
    }
};
Q_DECLARE_METATYPE(MacroConfig)

// 图标 SVG 常量 (#FF8C00 适配)
static const char* SVG_PANEL_RIGHT = 
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"white\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\"/><path d=\"M15 3v18\"/><path d=\"m8 9 3 3-3 3\"/></svg>";

static const char* SVG_SIDEBAR_OPEN = 
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"white\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\"/><path d=\"M15 3v18\"/><path d=\"m11 15-3-3 3-3\"/></svg>";

static const char* SVG_TRASH = 
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FF8C00\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><polyline points=\"3 6 5 6 21 6\" /><path d=\"M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2\" /><line x1=\"10\" y1=\"11\" x2=\"10\" y2=\"17\" /><line x1=\"14\" y1=\"11\" x2=\"14\" y2=\"17\" /></svg>";

static const char* SVG_CLOSE = 
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"white\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><line x1=\"18\" y1=\"6\" x2=\"6\" y2=\"18\"/><line x1=\"6\" y1=\"6\" x2=\"18\" y2=\"18\"/></svg>";

static const char* SVG_MINIMIZE = 
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"white\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><line x1=\"5\" y1=\"12\" x2=\"19\" y2=\"12\"/></svg>";

static const char* SVG_PIN_TILTED = 
    "<svg viewBox=\"0 0 24 24\" fill=\"white\" stroke=\"none\"><g transform=\"rotate(45 12 12)\"><path d=\"M16 9V4h1c.55 0 1-.45 1-1s-.45-1-1-1H7c-.55 0-1 .45-1 1s.45 1 1 1h1v5c0 1.66-1.34 3-3 3v2h5.97v7l1.03 1 1.03-1v-7H19v-2c-1.66 0-3-1.34-3-3z\"></path></g></svg>";

static const char* SVG_PIN_VERTICAL = 
    "<svg viewBox=\"0 0 24 24\" fill=\"#FF551C\" stroke=\"none\"><path d=\"M16 9V4h1c.55 0 1-.45 1-1s-.45-1-1-1H7c-.55 0-1 .45-1 1s.45 1 1 1h1v5c0 1.66-1.34 3-3 3v2h5.97v7l1.03 1 1.03-1v-7H19v-2c-1.66 0-3-1.34-3-3z\"></path></svg>";

static const char* SVG_SIDEBAR_TOGGLE = 
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"white\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\"/><path d=\"M15 3v18\"/><path d=\"m11 15-3-3 3-3\"/></svg>";

static const char* SVG_CALENDAR = 
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#0073FF\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><rect x=\"3\" y=\"4\" width=\"18\" height=\"18\" rx=\"2\" ry=\"2\"/><line x1=\"16\" y1=\"2\" x2=\"16\" y2=\"6\"/><line x1=\"8\" y1=\"2\" x2=\"8\" y2=\"6\"/><line x1=\"3\" y1=\"10\" x2=\"21\" y2=\"10\"/></svg>";

static const char* SVG_ZAP = 
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#3A90FF\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><polygon points=\"13 2 3 14 12 14 11 22 21 10 12 10 13 2\"/></svg>";

class KeyboardHook : public QObject {
    Q_OBJECT
public:
    static KeyboardHook& instance() {
        static KeyboardHook inst;
        return inst;
    }

    void start() {
#ifdef Q_OS_WIN
        if (m_hHook) return;
        m_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, HookProc, GetModuleHandle(NULL), 0);
#endif
    }

    void stop() {
#ifdef Q_OS_WIN
        if (m_hHook) {
            UnhookWindowsHookEx(m_hHook);
            m_hHook = nullptr;
        }
#endif
    }

    enum DigitSource { MainKeys, NumpadKeys, Both };

    void setDigitInterceptEnabled(bool enabled) { m_digitInterceptEnabled = enabled; }
    void setPaused(bool paused) { m_isPaused = paused; }
    bool isPaused() const { return m_isPaused; }
    
    void setDigitSource(DigitSource source) { m_digitSource = source; }
    DigitSource digitSource() const { return m_digitSource; }

    void setHotkey(const HotkeyConfig& config) { m_hotkey = config; }
    HotkeyConfig hotkey() const { return m_hotkey; }

    void setRecordingMode(bool recording) { 
        m_isRecording = recording; 
        if (recording) {
            m_ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000);
            m_altPressed = (GetAsyncKeyState(VK_MENU) & 0x8000);
            m_shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000);
        }
    }

signals:
    void digitPressed(int digit);
    void pauseToggled();
    void hotkeyRecorded(HotkeyConfig config);
    void exitRequested();
    void macroRecorded(MacroConfig config);
    void macroRecordingStateChanged(bool recording);

private:
    KeyboardHook() : m_hHook(nullptr), m_digitInterceptEnabled(false), m_isPaused(false), m_isRecording(false),
                     m_ctrlPressed(false), m_altPressed(false), m_shiftPressed(false), m_digitSource(Both),
                     m_isMacroRecording(false), m_countdown(0) {
        m_hotkey.vk = VK_F1;
        m_hotkey.name = "F1";
        
        m_macroTimer = new QTimer(this);
        connect(m_macroTimer, &QTimer::timeout, this, [this](){
            if (m_countdown > 1) {
                m_countdown--;
                QToolTip::showText(QCursor::pos(), QString("录制准备: %1").arg(m_countdown));
            } else {
                m_macroTimer->stop();
                m_isMacroRecording = true;
                m_recordedKeys.clear();
                QToolTip::showText(QCursor::pos(), "开始录制宏 (按下目标键)");
                emit macroRecordingStateChanged(true);
            }
        });
    }
    ~KeyboardHook() { stop(); }

#ifdef Q_OS_WIN
    static LRESULT CALLBACK HookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode == HC_ACTION) {
            KBDLLHOOKSTRUCT* pKey = (KBDLLHOOKSTRUCT*)lParam;
            bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

            // 实时跟踪修饰键状态 (解决录制时吞噬按键导致 GetAsyncKeyState 状态不更新的问题)
            if (pKey->vkCode == VK_LCONTROL || pKey->vkCode == VK_RCONTROL || pKey->vkCode == VK_CONTROL) {
                KeyboardHook::instance().m_ctrlPressed = isKeyDown;
            } else if (pKey->vkCode == VK_LMENU || pKey->vkCode == VK_RMENU || pKey->vkCode == VK_MENU) {
                KeyboardHook::instance().m_altPressed = isKeyDown;
            } else if (pKey->vkCode == VK_LSHIFT || pKey->vkCode == VK_RSHIFT || pKey->vkCode == VK_SHIFT) {
                KeyboardHook::instance().m_shiftPressed = isKeyDown;
            }
            
            bool ctrl = KeyboardHook::instance().m_ctrlPressed;
            bool alt = KeyboardHook::instance().m_altPressed;
            bool shift = KeyboardHook::instance().m_shiftPressed;

            if (!(pKey->flags & LLKHF_INJECTED)) {
                // 0. Ctrl + F1 宏录制开关 (优先级最高)
                if (ctrl && pKey->vkCode == VK_F1 && isKeyDown) { // 仅在按下时切换，防止双击触发
                    if (KeyboardHook::instance().m_isMacroRecording || KeyboardHook::instance().m_macroTimer->isActive()) {
                        // 停止录制
                        KeyboardHook::instance().m_macroTimer->stop();
                        if (KeyboardHook::instance().m_isMacroRecording && !KeyboardHook::instance().m_recordedKeys.isEmpty()) {
                            MacroConfig mc = MacroConfig::createNew(KeyboardHook::instance().m_recordedKeys);
                            emit KeyboardHook::instance().macroRecorded(mc);
                        }
                        KeyboardHook::instance().m_isMacroRecording = false;
                        QToolTip::showText(QCursor::pos(), "宏录制已停止");
                        emit KeyboardHook::instance().macroRecordingStateChanged(false);
                    } else {
                        // 启动倒计时
                        KeyboardHook::instance().m_recordedKeys.clear();
                        KeyboardHook::instance().m_countdown = 3;
                        QToolTip::showText(QCursor::pos(), "录制准备: 3");
                        KeyboardHook::instance().m_macroTimer->start(1000);
                    }
                    return 1;
                }

                // 宏录制执行逻辑
                if (KeyboardHook::instance().m_isMacroRecording) {
                    if (isKeyDown) { // 关键：只记录按下事件
                        // 过滤修饰键本身和启动热键
                        if (pKey->vkCode != VK_CONTROL && pKey->vkCode != VK_LCONTROL && pKey->vkCode != VK_RCONTROL &&
                            pKey->vkCode != VK_MENU && pKey->vkCode != VK_LMENU && pKey->vkCode != VK_RMENU &&
                            pKey->vkCode != VK_SHIFT && pKey->vkCode != VK_LSHIFT && pKey->vkCode != VK_RSHIFT &&
                            !(pKey->vkCode == VK_F1 && ctrl))
                        {
                            KeyboardHook::instance().m_recordedKeys.append(pKey->vkCode);
                            QToolTip::showText(QCursor::pos(), QString("当前录制序列步数: %1").arg(KeyboardHook::instance().m_recordedKeys.size()));
                        }
                    }
                    return 1;
                }

                // 1. 录制模式补丁：强制吞噬并转发，防止泄露给其他后台应用
                if (KeyboardHook::instance().m_isRecording) {
                    if (isKeyDown) {
                        // 过滤掉单纯的修饰键按下
                        if (pKey->vkCode != VK_CONTROL && pKey->vkCode != VK_LCONTROL && pKey->vkCode != VK_RCONTROL &&
                            pKey->vkCode != VK_MENU && pKey->vkCode != VK_LMENU && pKey->vkCode != VK_RMENU &&
                            pKey->vkCode != VK_SHIFT && pKey->vkCode != VK_LSHIFT && pKey->vkCode != VK_RSHIFT) 
                        {
                            HotkeyConfig cfg;
                            cfg.vk = pKey->vkCode;
                            cfg.ctrl = ctrl;
                            cfg.alt = alt;
                            cfg.shift = shift;
                            
                            QStringList parts;
                            if (cfg.ctrl) parts << "Ctrl";
                            if (cfg.alt) parts << "Alt";
                            if (cfg.shift) parts << "Shift";
                            
                            // 简单映射常见按键名
                            QString name;
                            if (cfg.vk >= 'A' && cfg.vk <= 'Z') name = QString(QChar(cfg.vk));
                            else if (cfg.vk >= '0' && cfg.vk <= '9') name = QString(QChar(cfg.vk));
                            else if (cfg.vk >= VK_F1 && cfg.vk <= VK_F24) name = QString("F%1").arg(cfg.vk - VK_F1 + 1);
                            else {
                                switch(cfg.vk) {
                                    case VK_SPACE: name = "Space"; break;
                                    case VK_RETURN: name = "Enter"; break;
                                    case VK_ESCAPE: name = "Esc"; break;
                                    case VK_OEM_3: name = "~"; break;
                                    default: name = QString("Key_%1").arg(cfg.vk); break;
                                }
                            }
                            parts << name;
                            cfg.name = parts.join("+");
                            
                            emit KeyboardHook::instance().hotkeyRecorded(cfg);
                        }
                    }
                    return 1; 
                }

                // 2. Ctrl + W 全局退出 (仅在拦截模式开启时生效，防止非预期全局抢占)
                if (KeyboardHook::instance().m_digitInterceptEnabled && ctrl && pKey->vkCode == 'W' && isKeyDown) {
                    emit KeyboardHook::instance().exitRequested();
                    return 1;
                }

                // 3. 匹配热键
                const auto& target = KeyboardHook::instance().m_hotkey;
                if (pKey->vkCode == target.vk && ctrl == target.ctrl && alt == target.alt && shift == target.shift) {
                    if (isKeyDown) {
                        emit KeyboardHook::instance().pauseToggled();
                    }
                    return 1;
                }

                // 4. 数字拦截
                if (KeyboardHook::instance().m_digitInterceptEnabled && !KeyboardHook::instance().m_isPaused) {
                    bool isMain = (pKey->vkCode >= 0x30 && pKey->vkCode <= 0x39);
                    bool isNum  = (pKey->vkCode >= 0x60 && pKey->vkCode <= 0x69);
                    
                    if ((isMain || isNum) && !ctrl && !alt) {
                        DigitSource src = KeyboardHook::instance().m_digitSource;
                        bool allow = false;
                        if (src == Both) allow = true;
                        else if (src == MainKeys && isMain) allow = true;
                        else if (src == NumpadKeys && isNum) allow = true;

                        if (allow) {
                            if (isKeyDown) {
                                int digit = isMain ? (pKey->vkCode - 0x30) : (pKey->vkCode - 0x60);
                                emit KeyboardHook::instance().digitPressed(digit);
                            }
                            return 1;
                        }
                    }
                }
            }
        }
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }
    HHOOK m_hHook;
#endif
    bool m_digitInterceptEnabled;
    bool m_isPaused;
    DigitSource m_digitSource;
    HotkeyConfig m_hotkey;
    bool m_isRecording;
    bool m_ctrlPressed, m_altPressed, m_shiftPressed;

    bool m_isMacroRecording;
    QVector<int> m_recordedKeys;
    int m_countdown;
    QTimer* m_macroTimer;
};

// ============================================================================
// StateIconBtn: 状态图标按钮 (标题栏专用)
// ============================================================================
class StateIconBtn : public QPushButton {
    Q_OBJECT
public:
    explicit StateIconBtn(QWidget* parent = nullptr) : QPushButton(parent), m_isPaused(false) {
        setFixedSize(24, 24);
        setCursor(Qt::PointingHandCursor);
        setStyleSheet("QPushButton { background: transparent; border: none; border-radius: 4px; }");
    }

    void setPaused(bool paused) {
        m_isPaused = paused;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        
        if (isDown()) {
            painter.setBrush(QColor("#555555"));
        } else if (underMouse()) {
            painter.setBrush(QColor("#444444"));
        } else {
            painter.setBrush(Qt::transparent);
        }
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(rect(), 4, 4);

        int side = 14;
        int x = (width() - side) / 2;
        int y = (height() - side) / 2;

        painter.setBrush(QColor("#FF8C00"));
        if (m_isPaused) {
            QPainterPath path;
            path.moveTo(x, y);
            path.lineTo(x + side, y + (float)side / 2.0);
            path.lineTo(x, y + side);
            path.closeSubpath();
            painter.drawPath(path);
        } else {
            painter.drawRect(x, y, 5, side);
            painter.drawRect(x + side - 5, y, 5, side);
        }
    }

private:
    bool m_isPaused;
};

// ============================================================================
// HotkeyEdit: 专业热键编辑器 (修复焦点与按键泄露)
// ============================================================================
class HotkeyEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit HotkeyEdit(QWidget* parent = nullptr) : QLineEdit(parent) {
        setReadOnly(true);
        setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        setStyleSheet(
            "QLineEdit {"
            "  color: #FF8C00;"
            "  background-color: transparent;"
            "  border: none;"
            "  font-size: 12px;"
            "  font-weight: bold;"
            "}"
        );
        connect(&KeyboardHook::instance(), &KeyboardHook::hotkeyRecorded, this, &HotkeyEdit::onGlobalKeyRecorded);
    }

signals:
    void hotkeyChanged(HotkeyConfig config);

private slots:
    void onGlobalKeyRecorded(HotkeyConfig cfg) {
        if (!hasFocus()) return;
        setText(cfg.name);
        emit hotkeyChanged(cfg);
        clearFocus();
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        setFocus();
        QLineEdit::mousePressEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        // 全局钩子已接管录制，此处仅屏蔽默认行为
        if (event->key() == Qt::Key_Escape) clearFocus();
    }

    void focusInEvent(QFocusEvent* event) override {
        QLineEdit::focusInEvent(event);
        setText("请按下组合键...");
#ifdef Q_OS_WIN
        HWND hwnd = (HWND)window()->winId();
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, GetWindowLongPtr(hwnd, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
#endif
        KeyboardHook::instance().setRecordingMode(true);
    }

    void focusOutEvent(QFocusEvent* event) override {
        QLineEdit::focusOutEvent(event);
        KeyboardHook::instance().setRecordingMode(false);
        HotkeyConfig cfg = KeyboardHook::instance().hotkey();
        setText(cfg.name);
#ifdef Q_OS_WIN
        HWND hwnd = (HWND)window()->winId();
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, GetWindowLongPtr(hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
#endif
    }
};

// ============================================================================
// FramelessDialog: 基础无边框窗口 (重构宏管理)
// ============================================================================
class FramelessDialog : public QDialog {
    Q_OBJECT
public:
    explicit FramelessDialog(const QString& title, QWidget* parent = nullptr) 
        : QDialog(parent, Qt::FramelessWindowHint | Qt::Window), m_isStayOnTop(true) 
    {
        setAttribute(Qt::WA_TranslucentBackground);
        
        m_outerLayout = new QVBoxLayout(this);
        m_outerLayout->setContentsMargins(12, 12, 12, 12);

        m_container = new QWidget(this);
        m_container->setObjectName("DialogContainer");
        m_container->setStyleSheet(
            "#DialogContainer {"
            "  background-color: #1e1e1e;"
            "  border: 1px solid #333;"
            "  border-radius: 10px;"
            "}"
        );
        m_outerLayout->addWidget(m_container);

        auto* shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(15);
        shadow->setXOffset(0); shadow->setYOffset(2);
        shadow->setColor(QColor(0, 0, 0, 90));
        m_container->setGraphicsEffect(shadow);

        // 主容器采用垂直布局，顶部为工具栏，下方为内容
        auto* containerLayout = new QVBoxLayout(m_container);
        containerLayout->setContentsMargins(1, 1, 1, 1);
        containerLayout->setSpacing(0);

        // --- 顶部工具栏 (整合 Logo、标题与功能按钮) ---
        m_toolbar = new QWidget();
        m_toolbar->setObjectName("Toolbar");
        m_toolbar->setFixedHeight(36);
        m_toolbar->setStyleSheet(
            "QWidget#Toolbar { background-color: #252526; border-top-left-radius: 9px; border-top-right-radius: 9px; border: none; border-bottom: 1px solid #333; }"
            "QPushButton { border: none; border-radius: 4px; background: transparent; }"
            "QPushButton:hover, QPushButton:checked { background-color: #3e3e42; }"
            "QPushButton#CloseBtn { background-color: #E81123; color: white; }"
            "QPushButton#CloseBtn:hover { background-color: #D71520; }"
        );
        auto* toolLayout = new QHBoxLayout(m_toolbar);
        toolLayout->setContentsMargins(6, 0, 6, 0);
        toolLayout->setSpacing(4);

        // 添加 Logo
        auto* logoLabel = new QLabel();
        logoLabel->setFixedSize(24, 24);
        QSvgRenderer* logoRenderer = new QSvgRenderer(QString(":/icons/app_logo.svg"), this);
        QPixmap logoPix(24, 24);
        logoPix.fill(Qt::transparent);
        QPainter logoPainter(&logoPix);
        logoRenderer->render(&logoPainter);
        logoLabel->setPixmap(logoPix);
        toolLayout->addWidget(logoLabel);

        // 添加标题文字
        auto* titleLabel = new QLabel(title);
        titleLabel->setStyleSheet("color: #FF8C00; font-size: 14px; font-weight: bold; border: none; margin-left: 1px;");
        toolLayout->addWidget(titleLabel);

        toolLayout->addStretch(); // 弹簧，将功能按钮推向右侧

        auto createToolBtn = [&](const char* svg, const QString& tip, const QString& objName = QString()) {
            auto* btn = new QPushButton();
            btn->setAutoDefault(false);
            btn->setDefault(false);
            if (!objName.isEmpty()) btn->setObjectName(objName);
            btn->setFixedSize(24, 24);
            btn->setCursor(Qt::PointingHandCursor);
            QSvgRenderer* r = new QSvgRenderer(QByteArray(svg), this);
            QPixmap pix(18, 18); pix.fill(Qt::transparent);
            QPainter p(&pix); r->render(&p);
            btn->setIcon(QIcon(pix));
            btn->setIconSize(QSize(18, 18));
            btn->setToolTip(tip);
            return btn;
        };

        m_calendarBtn = createToolBtn(SVG_CALENDAR, "日期");
        connect(m_calendarBtn, &QPushButton::clicked, this, &FramelessDialog::onCalendarBtnClicked);
        toolLayout->addWidget(m_calendarBtn);

        m_stateBtn = new StateIconBtn();
        connect(m_stateBtn, &QPushButton::clicked, this, &FramelessDialog::onStateBtnClicked);
        toolLayout->addWidget(m_stateBtn);

        m_sidebarBtn = createToolBtn(SVG_SIDEBAR_TOGGLE, "宏管理");
        toolLayout->addWidget(m_sidebarBtn);

        m_pinBtn = createToolBtn(SVG_PIN_VERTICAL, "置顶");
        m_pinBtn->setCheckable(true);
        m_pinBtn->setChecked(true);
        connect(m_pinBtn, &QPushButton::clicked, this, &FramelessDialog::onPinBtnClicked);
        toolLayout->addWidget(m_pinBtn);

        auto* minBtn = createToolBtn(SVG_MINIMIZE, "最小化");
        connect(minBtn, &QPushButton::clicked, this, &FramelessDialog::showMinimized);
        toolLayout->addWidget(minBtn);

        auto* closeBtn = createToolBtn(SVG_CLOSE, "关闭", "CloseBtn");
        connect(closeBtn, &QPushButton::clicked, this, &QApplication::quit);
        toolLayout->addWidget(closeBtn);

        containerLayout->addWidget(m_toolbar);

        // --- 内容区 ---
        m_contentArea = new QWidget();
        m_contentArea->setStyleSheet("background: transparent; border: none;");
        containerLayout->addWidget(m_contentArea, 1);

        setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    }

protected:
    void onCalendarBtnClicked() {
        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background-color: #2D2D2D; color: #BBB; border: 1px solid #444; padding: 4px; } "
            "QMenu::item { padding: 6px 24px; border-radius: 4px; } "
            "QMenu::item:selected { background-color: #3e3e42; color: #FF8C00; } "
            "QMenu::separator { height: 1px; background: #444; margin: 2px 0; }"
        );

        QDate today = QDate::currentDate();
        QStringList formats = {
            "dd-MM-yyyy", "yyyy-MM-dd", "yyyy年MM月dd日", "dd日MM月yyyy年",
            "yyyy/MM/dd", "dd/MM/yyyy", "yyyyMMdd", "ddMMyyyy"
        };

        for (int i = 0; i < formats.size(); ++i) {
            QString dateStr = today.toString(formats[i]);
            QAction* action = menu.addAction(dateStr);
            connect(action, &QAction::triggered, this, [dateStr]() {
                QGuiApplication::clipboard()->setText(dateStr);
            });
            if (i < formats.size() - 1) {
                menu.addSeparator();
            }
        }

        menu.exec(m_calendarBtn->mapToGlobal(QPoint(0, m_calendarBtn->height())));
    }

    void onStateBtnClicked() { emit stateToggleRequested(); }
    void onPinBtnClicked() {
        m_isStayOnTop = m_pinBtn->isChecked();
        
        QSvgRenderer renderer(QByteArray(m_isStayOnTop ? SVG_PIN_VERTICAL : SVG_PIN_TILTED));
        QPixmap pix(18, 18);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        renderer.render(&p);
        m_pinBtn->setIcon(QIcon(pix));
        
#ifdef Q_OS_WIN
        HWND hwnd = (HWND)winId();
        SetWindowPos(hwnd, m_isStayOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
        setWindowFlag(Qt::WindowStaysOnTopHint, m_isStayOnTop);
        show();
#endif
    }

#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override {
        MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_NCHITTEST) {
            int x = GET_X_LPARAM(msg->lParam);
            int y = GET_Y_LPARAM(msg->lParam);
            QPoint pos = mapFromGlobal(QPoint(x, y));
            
            // [UX-FIX] 优化性能：直接获取鼠标下的控件，排除交互式控件，防止拖拽逻辑拦截点击事件
            QWidget* child = childAt(pos);
            if (child) {
                // 向上查找，判断该控件或其父控件是否属于交互类
                QWidget* w = child;
                while (w && w != this) {
                    if (qobject_cast<QAbstractButton*>(w) || qobject_cast<QLineEdit*>(w) || qobject_cast<QAbstractSlider*>(w)) {
                        return false; 
                    }
                    w = w->parentWidget();
                }
            }

            if (pos.y() < 48) { // 涵盖标题栏区域 (12px 边距 + 36px 标题栏)
                *result = HTCAPTION;
                return true;
            }
        }
        return QDialog::nativeEvent(eventType, message, result);
    }
#endif

signals:
    void stateToggleRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return) {
            event->ignore();
            return;
        }
        QDialog::keyPressEvent(event);
    }

protected:
    QWidget* m_container;
    QVBoxLayout* m_outerLayout;
    QWidget* m_contentArea;
    QWidget* m_toolbar;
    StateIconBtn* m_stateBtn;
    QPushButton* m_calendarBtn;
    QPushButton* m_sidebarBtn;
    QPushButton* m_pinBtn;
    bool m_isStayOnTop;
};

// ============================================================================
// MacroItemWidget: 宏列表项 (支持单选、编辑、删除)
// ============================================================================
class MacroItemWidget : public QWidget {
    Q_OBJECT
public:
    explicit MacroItemWidget(const MacroConfig& cfg, QWidget* parent = nullptr) : QWidget(parent), m_config(cfg) {
        setFixedHeight(40);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(10, 0, 10, 0);
        layout->setSpacing(10);

        m_check = new QCheckBox();
        m_check->setChecked(cfg.active);
        m_check->setStyleSheet(
            "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px; border: 2px solid #444; background: #252525; } "
            "QCheckBox::indicator:checked { background-color: #FF8C00; border: 2px solid #FF8C00; }"
        );
        layout->addWidget(m_check);

        m_nameEdit = new QLineEdit(cfg.name);
        m_nameEdit->setReadOnly(true);
        m_nameEdit->installEventFilter(this); // 安装过滤器以捕获双击
        m_nameEdit->setStyleSheet("color: #BBB; background: transparent; border: none; font-size: 13px;");
        layout->addWidget(m_nameEdit, 1);

        QStringList keyNames;
        for (int vk : cfg.keys) {
            if (vk == VK_TAB) keyNames << "Tab";
            else if (vk == VK_RETURN) keyNames << "Enter";
            else if (vk == VK_SPACE) keyNames << "Space";
            else if (vk >= 'A' && vk <= 'Z') keyNames << QString(QChar(vk));
            else keyNames << QString("K_%1").arg(vk);
        }
        setToolTip("录制序列: " + keyNames.join(" → "));

        m_delBtn = new QPushButton();
        m_delBtn->setAutoDefault(false);
        m_delBtn->setDefault(false);
        m_delBtn->setFixedSize(24, 24);
        m_delBtn->setCursor(Qt::PointingHandCursor);
        m_delBtn->setStyleSheet("QPushButton { background: transparent; border: none; } QPushButton:hover { background: rgba(255, 0, 0, 0.1); border-radius: 4px; }");
        
        QSvgRenderer* renderer = new QSvgRenderer(QByteArray(SVG_TRASH), this);
        QPixmap pix(18, 18); pix.fill(Qt::transparent);
        QPainter painter(&pix); renderer->render(&painter);
        m_delBtn->setIcon(QIcon(pix));
        
        layout->addWidget(m_delBtn);

        connect(m_check, &QCheckBox::clicked, this, [this](bool checked){
            m_config.active = checked;
            emit stateChanged();
        });
        connect(m_nameEdit, &QLineEdit::editingFinished, this, [this](){
            m_nameEdit->setReadOnly(true);
            m_nameEdit->setStyleSheet("color: #BBB; background: transparent; border: none; font-size: 13px;");
            m_config.name = m_nameEdit->text();
            
#ifdef Q_OS_WIN
            // 2. 退出编辑：恢复“不抢焦点”状态
            HWND hwnd = (HWND)window()->winId();
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, GetWindowLongPtr(hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
#endif
            KeyboardHook::instance().setDigitInterceptEnabled(true); // 恢复钩子拦截
            
            emit stateChanged();
        });
        connect(m_delBtn, &QPushButton::clicked, this, &MacroItemWidget::deleteRequested);
    }

    void setChecked(bool checked) { m_check->setChecked(checked); m_config.active = checked; }
    bool isChecked() const { return m_check->isChecked(); }
    MacroConfig config() const { return m_config; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_nameEdit && event->type() == QEvent::MouseButtonDblClick) {
#ifdef Q_OS_WIN
            // 1. 进入编辑：解除“不抢焦点”限制，允许输入
            HWND hwnd = (HWND)window()->winId();
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, GetWindowLongPtr(hwnd, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE);
            SetForegroundWindow(hwnd);
#endif
            KeyboardHook::instance().setDigitInterceptEnabled(false); // 暂时静默钩子
            
            m_nameEdit->setReadOnly(false);
            m_nameEdit->setStyleSheet("color: #FFF; background: #333; border: 1px solid #FF8C00; border-radius: 2px; font-size: 13px;");
            m_nameEdit->setFocus();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

signals:
    void stateChanged();
    void deleteRequested();

private:
    MacroConfig m_config;
    QCheckBox* m_check;
    QLineEdit* m_nameEdit;
    QPushButton* m_delBtn;
};

// ============================================================================
// TimePasteWindow: 主窗口 (极简布局 + 绝对底部热键栏)
// ============================================================================
class TimePasteWindow : public FramelessDialog {
    Q_OBJECT
public:
    explicit TimePasteWindow(QWidget* parent = nullptr) : FramelessDialog("时间工具", parent) {
        // 核心尺寸计算：主内容 380 + 外层布局边距 12*2 = 404 (宏管理展开 +240)
        // 调整：高度减去 15px (484 -> 469)
        setFixedSize(404, 469); 
        // [修复] 设置窗口图标，确保任务栏及窗口系统菜单显示一致
        QIcon appIcon(":/icons/app_logo.svg");
        if (appIcon.isNull()) {
            appIcon = QIcon(":/app_icon.ico");
        }
        setWindowIcon(appIcon);
        initUI();

        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &TimePasteWindow::updateDateTime);
        m_timer->start(100);

        connect(&KeyboardHook::instance(), &KeyboardHook::digitPressed, this, &TimePasteWindow::onDigitPressed, Qt::QueuedConnection);
        connect(&KeyboardHook::instance(), &KeyboardHook::pauseToggled, this, &TimePasteWindow::togglePause, Qt::QueuedConnection);
        connect(&KeyboardHook::instance(), &KeyboardHook::exitRequested, qApp, &QApplication::quit, Qt::QueuedConnection);
        connect(this, &FramelessDialog::stateToggleRequested, this, &TimePasteWindow::togglePause);
        
        connect(m_sidebarBtn, &QPushButton::clicked, this, &TimePasteWindow::toggleSidebar);
        connect(&KeyboardHook::instance(), &KeyboardHook::macroRecorded, this, &TimePasteWindow::onMacroRecorded);
        
        loadSettings();
        updateSidebarIcon();
    }

    void toggleSidebar() {
        bool isExpanded = (width() > 500);
        bool willExpand = !isExpanded;
        m_sidebar->setVisible(willExpand);
        // 收起：404, 展开：404 + 240 = 644。高度保持 469
        setFixedSize(willExpand ? 644 : 404, 469);
        
        // 动态更新边框与圆角逻辑，确保 1 像素分割线及外边框完美衔接
        if (willExpand) {
            // 展开时：右下角圆角设为 0 以衔接宏管理
            m_hotkeyBar->setStyleSheet("background-color: #222; border: none; border-bottom-left-radius: 9px; border-bottom-right-radius: 0px;");
        } else {
            // 收起时：右下角恢复 9px 圆角
            m_hotkeyBar->setStyleSheet("background-color: #222; border: none; border-bottom-left-radius: 9px; border-bottom-right-radius: 9px;");
        }
        
        updateSidebarIcon();
    }

    void updateSidebarIcon() {
        bool isExpanded = (width() > 500);
        QSvgRenderer renderer(QByteArray(isExpanded ? SVG_PANEL_RIGHT : SVG_SIDEBAR_TOGGLE));
        QPixmap pix(18, 18); pix.fill(Qt::transparent);
        QPainter painter(&pix);
        renderer.render(&painter);
        m_sidebarBtn->setIcon(QIcon(pix));
        m_sidebarBtn->setIconSize(QSize(18, 18));
    }

    void onMacroRecorded(MacroConfig mc) {
        addMacroItem(mc);
        if (!(width() > 500)) {
            m_sidebar->show();
            setFixedSize(644, 469);
            updateSidebarIcon();
        }
        saveSettings();
    }

    void addMacroItem(const MacroConfig& cfg) {
        auto* item = new MacroItemWidget(cfg, m_macroListContent);
        m_macroListLayout->insertWidget(m_macroListLayout->count() - 1, item);
        
        connect(item, &MacroItemWidget::stateChanged, this, [this, item](){
            if (item->isChecked()) {
                for (int i = 0; i < m_macroListLayout->count() - 1; ++i) {
                    auto* other = qobject_cast<MacroItemWidget*>(m_macroListLayout->itemAt(i)->widget());
                    if (other && other != item) other->setChecked(false);
                }
            }
            saveSettings();
        });
        connect(item, &MacroItemWidget::deleteRequested, this, [this, item](){
            item->deleteLater();
            QTimer::singleShot(100, this, &TimePasteWindow::saveSettings);
        });
    }

protected:
    void showEvent(QShowEvent* event) override {
        FramelessDialog::showEvent(event);
        KeyboardHook::instance().setDigitInterceptEnabled(true);
#ifdef Q_OS_WIN
        HWND hwnd = (HWND)winId();
        LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE | WS_EX_APPWINDOW);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
#endif
    }

    void hideEvent(QHideEvent* event) override {
        KeyboardHook::instance().setDigitInterceptEnabled(false);
        saveSettings();
        FramelessDialog::hideEvent(event);
    }

private slots:
    void onHotkeyChanged(HotkeyConfig cfg) {
        KeyboardHook::instance().setHotkey(cfg);
    }

    void togglePause() {
        bool newState = !KeyboardHook::instance().isPaused();
        KeyboardHook::instance().setPaused(newState);
        m_stateBtn->setPaused(newState);
        m_timeLabel->setStyleSheet(newState ? "color: #666; font-size: 32px; font-weight: bold; font-family: 'Consolas';" : "color: #FF8C00; font-size: 32px; font-weight: bold; font-family: 'Consolas';");
    }

    void updateDateTime() {
        QDateTime now = QDateTime::currentDateTime();
        m_dateLabel->setText(now.toString("yyyy-MM-dd"));
        m_timeLabel->setText(now.toString("HH:mm:ss"));
    }

    void onDigitPressed(int digit) {
        if (!isVisible() || KeyboardHook::instance().isPaused()) return;
        QDateTime target = QDateTime::currentDateTime().addSecs((m_radioPrev->isChecked() ? -digit : digit) * 60);
        QString timeStr = target.toString("HH:mm");

        // 获取选中的宏
        MacroConfig activeMacro;
        bool hasMacro = false;
        for (int i = 0; i < m_macroListLayout->count() - 1; ++i) {
            auto* item = qobject_cast<MacroItemWidget*>(m_macroListLayout->itemAt(i)->widget());
            if (item && item->isChecked()) {
                activeMacro = item->config();
                hasMacro = true;
                break;
            }
        }

#ifdef Q_OS_WIN
        QTimer::singleShot(10, [timeStr, hasMacro, activeMacro]() {
            // 1. 释放所有修饰键
            INPUT releaseInputs[8]; memset(releaseInputs, 0, sizeof(releaseInputs));
            BYTE keys[] = { VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
            for (int i = 0; i < 8; ++i) { releaseInputs[i].type = INPUT_KEYBOARD; releaseInputs[i].ki.wVk = keys[i]; releaseInputs[i].ki.dwFlags = KEYEVENTF_KEYUP; }
            SendInput(8, releaseInputs, sizeof(INPUT));

            // 2. 发送时间字符串
            int len = timeStr.length(); QVector<INPUT> inputs(len * 2);
            for (int i = 0; i < len; ++i) {
                inputs[i*2].type = INPUT_KEYBOARD; inputs[i*2].ki.wScan = timeStr[i].unicode(); inputs[i*2].ki.dwFlags = KEYEVENTF_UNICODE;
                inputs[i*2+1] = inputs[i*2]; inputs[i*2+1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            }
            SendInput(inputs.size(), inputs.data(), sizeof(INPUT));

            // 3. 执行宏 (异步延时，防止 Sleep 阻塞 GUI 线程导致界面卡死)
            if (hasMacro && !activeMacro.keys.isEmpty()) {
                QTimer::singleShot(50, [activeMacro]() {
                    QVector<INPUT> macroInputs;
                    for (int vk : activeMacro.keys) {
                        INPUT down = {0}, up = {0};
                        down.type = INPUT_KEYBOARD; down.ki.wVk = vk;
                        up = down; up.ki.dwFlags = KEYEVENTF_KEYUP;
                        macroInputs << down << up;
                    }
                    if (!macroInputs.isEmpty()) {
                        SendInput(macroInputs.size(), macroInputs.data(), sizeof(INPUT));
                    }
                });
            }
        });
#endif
    }

private:
    void initUI() {
        // 使用水平布局容纳左侧面板和右侧宏管理
        auto* mainArea = new QHBoxLayout(m_contentArea);
        mainArea->setContentsMargins(0, 0, 0, 0);
        mainArea->setSpacing(0);

        auto* leftPanel = new QWidget();
        leftPanel->setFixedWidth(380);
        leftPanel->setStyleSheet("background: transparent; border: none;");
        auto* lpLayout = new QVBoxLayout(leftPanel);
        lpLayout->setContentsMargins(0, 0, 0, 0);
        lpLayout->setSpacing(0);

        auto* innerContent = new QWidget();
        innerContent->setStyleSheet("background: transparent; border: none;");
        auto* contentLayout = new QVBoxLayout(innerContent);
        contentLayout->setContentsMargins(25, 15, 25, 0);
        contentLayout->setSpacing(15);

        m_dateLabel = new QLabel(); m_dateLabel->setAlignment(Qt::AlignCenter);
        m_dateLabel->setStyleSheet("color: #888; font-size: 16px;");
        contentLayout->addWidget(m_dateLabel);

        m_timeLabel = new QLabel(); m_timeLabel->setAlignment(Qt::AlignCenter);
        m_timeLabel->setStyleSheet("color: #FF8C00; font-size: 32px; font-weight: bold; font-family: 'Consolas';");
        contentLayout->addWidget(m_timeLabel);

        auto* sep = new QLabel(); sep->setFixedHeight(1); sep->setStyleSheet("background-color: #333;");
        contentLayout->addWidget(sep);

        auto radioStyle = "QRadioButton, QCheckBox { color: #BBB; font-size: 14px; spacing: 10px; } "
                          "QRadioButton::indicator, QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px; border: 2px solid #444; background: #252525; } "
                          "QRadioButton::indicator:checked, QCheckBox::indicator:checked { background-color: #FF8C00; border: 2px solid #FF8C00; }";

        m_radioPrev = new QRadioButton("退 (往前 N 分钟)"); m_radioPrev->setChecked(true);
        m_radioPrev->setStyleSheet(radioStyle);
        contentLayout->addWidget(m_radioPrev);

        m_radioNext = new QRadioButton("进 (往后 N 分钟)");
        m_radioNext->setStyleSheet(radioStyle);
        contentLayout->addWidget(m_radioNext);

        m_buttonGroup = new QButtonGroup(this);
        m_buttonGroup->addButton(m_radioPrev, 0); m_buttonGroup->addButton(m_radioNext, 1);

        auto* line2 = new QLabel(); line2->setFixedHeight(1); line2->setStyleSheet("background-color: #333;");
        contentLayout->addWidget(line2);

        m_radioMain = new QRadioButton("仅主键盘数字键");
        m_radioMain->setStyleSheet(radioStyle);
        contentLayout->addWidget(m_radioMain);

        m_radioNumpad = new QRadioButton("仅小键盘数字键");
        m_radioNumpad->setStyleSheet(radioStyle);
        contentLayout->addWidget(m_radioNumpad);

        m_radioBoth = new QRadioButton("同时使用 (主/小键盘)");
        m_radioBoth->setStyleSheet(radioStyle);
        contentLayout->addWidget(m_radioBoth);

        m_sourceGroup = new QButtonGroup(this);
        m_sourceGroup->addButton(m_radioMain, (int)KeyboardHook::MainKeys);
        m_sourceGroup->addButton(m_radioNumpad, (int)KeyboardHook::NumpadKeys);
        m_sourceGroup->addButton(m_radioBoth, (int)KeyboardHook::Both);

        connect(m_sourceGroup, &QButtonGroup::idClicked, [](int id){
            KeyboardHook::instance().setDigitSource((KeyboardHook::DigitSource)id);
        });

        contentLayout->addSpacing(10); // 向下偏移 10 像素
        auto* tip = new QLabel("按 0-9 输出时间 | Ctrl+W 退出程序");
        tip->setAlignment(Qt::AlignCenter);
        tip->setStyleSheet("color: #555; font-size: 11px;");
        contentLayout->addWidget(tip);
        contentLayout->addStretch();

        lpLayout->addWidget(innerContent, 1);

        m_hotkeyBar = new QWidget();
        m_hotkeyBar->setFixedHeight(42);
        m_hotkeyBar->setStyleSheet("background-color: #222; border: none; border-bottom-left-radius: 9px; border-bottom-right-radius: 9px;");
        auto* hbLayout = new QHBoxLayout(m_hotkeyBar);
        hbLayout->setContentsMargins(20, 0, 20, 0);
        hbLayout->setSpacing(10);
        auto* label = new QLabel("自定义热键:");
        label->setStyleSheet("color: #888; font-size: 12px;");
        hbLayout->addWidget(label);

        m_hotkeyEdit = new HotkeyEdit();
        connect(m_hotkeyEdit, &HotkeyEdit::hotkeyChanged, this, &TimePasteWindow::onHotkeyChanged);
        hbLayout->addWidget(m_hotkeyEdit);
        hbLayout->addStretch();
        lpLayout->addWidget(m_hotkeyBar);

        mainArea->addWidget(leftPanel);

        // 右侧宏管理
        m_sidebar = new QWidget();
        m_sidebar->setFixedWidth(240);
        m_sidebar->setStyleSheet("background-color: transparent; border: none; border-left: 1px solid #333;");
        auto* rbLayout = new QVBoxLayout(m_sidebar);
        rbLayout->setContentsMargins(0, 0, 0, 0);
        rbLayout->setSpacing(0);

        auto* rbTitle = new QLabel("宏管理 (Ctrl+F1 录制)");
        rbTitle->setFixedHeight(40);
        rbTitle->setAlignment(Qt::AlignCenter);
        rbTitle->setStyleSheet("color: #888; font-size: 12px; font-weight: bold; border-bottom: 1px solid #333;");
        rbLayout->addWidget(rbTitle);

        auto* scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setAttribute(Qt::WA_TranslucentBackground);
        scroll->viewport()->setStyleSheet("background: transparent;");
        scroll->setStyleSheet(
            "QScrollArea { border: none; background: transparent; } "
            "QScrollBar:vertical { width: 6px; background: transparent; } "
            "QScrollBar::handle:vertical { background: #333; border-radius: 3px; min-height: 20px; } "
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        );
        
        m_macroListContent = new QWidget();
        m_macroListContent->setStyleSheet("background: transparent; border: none;");
        m_macroListLayout = new QVBoxLayout(m_macroListContent);
        m_macroListLayout->setContentsMargins(0, 0, 0, 0);
        m_macroListLayout->setSpacing(0);
        m_macroListLayout->addStretch();
        
        scroll->setWidget(m_macroListContent);
        rbLayout->addWidget(scroll, 1);

        auto* rbFooter = new QWidget();
        rbFooter->setFixedHeight(42);
        rbFooter->setStyleSheet("background-color: #222; border: none; border-bottom-right-radius: 9px;");
        rbLayout->addWidget(rbFooter);

        mainArea->addWidget(m_sidebar);
        m_sidebar->hide(); // 默认隐藏
    }

    void loadSettings() {
        QSettings s("RapidTimeTool", "Window");
        if (s.contains("pos")) move(s.value("pos").toPoint());
        m_buttonGroup->button(s.value("mode", 0).toInt())->setChecked(true);
        
        int srcId = s.value("digitSource", (int)KeyboardHook::Both).toInt();
        if (auto* b = m_sourceGroup->button(srcId)) b->setChecked(true);
        KeyboardHook::instance().setDigitSource((KeyboardHook::DigitSource)srcId);

        int macroCount = s.beginReadArray("macros");
        for (int i = 0; i < macroCount; ++i) {
            s.setArrayIndex(i);
            MacroConfig mc;
            mc.id = s.value("id").toString();
            mc.name = s.value("name").toString();
            QVariantList vKeys = s.value("keys").toList();
            for (const auto& v : vKeys) mc.keys << v.toInt();
            mc.active = s.value("active").toBool();
            addMacroItem(mc);
        }
        s.endArray();

        HotkeyConfig cfg;
        cfg.vk = s.value("hotkeyVK", VK_F1).toInt();
        cfg.ctrl = s.value("hotkeyCtrl", false).toBool();
        cfg.alt = s.value("hotkeyAlt", false).toBool();
        cfg.shift = s.value("hotkeyShift", false).toBool();
        cfg.name = s.value("hotkeyName", "F1").toString();
        KeyboardHook::instance().setHotkey(cfg);
        m_hotkeyEdit->setText(cfg.name);
    }

    void saveSettings() {
        QSettings s("RapidTimeTool", "Window");
        s.setValue("pos", pos());
        s.setValue("mode", m_buttonGroup->checkedId());
        s.setValue("digitSource", m_sourceGroup->checkedId());
        
        s.beginWriteArray("macros");
        int idx = 0;
        for (int i = 0; i < m_macroListLayout->count() - 1; ++i) {
            auto* item = qobject_cast<MacroItemWidget*>(m_macroListLayout->itemAt(i)->widget());
            if (item) {
                s.setArrayIndex(idx++);
                MacroConfig mc = item->config();
                s.setValue("id", mc.id);
                s.setValue("name", mc.name);
                QVariantList vKeys;
                for (int vk : mc.keys) vKeys << vk;
                s.setValue("keys", vKeys);
                s.setValue("active", mc.active);
            }
        }
        s.endArray();

        const auto& cfg = KeyboardHook::instance().hotkey();
        s.setValue("hotkeyVK", cfg.vk); s.setValue("hotkeyCtrl", cfg.ctrl);
        s.setValue("hotkeyAlt", cfg.alt); s.setValue("hotkeyShift", cfg.shift);
        s.setValue("hotkeyName", cfg.name);
    }

    QLabel *m_dateLabel, *m_timeLabel;
    QRadioButton *m_radioPrev, *m_radioNext;
    QRadioButton *m_radioMain, *m_radioNumpad, *m_radioBoth;
    QButtonGroup *m_buttonGroup, *m_sourceGroup;
    QTimer* m_timer;
    HotkeyEdit* m_hotkeyEdit;

    QWidget *m_sidebar, *m_macroListContent, *m_hotkeyBar;
    QVBoxLayout *m_macroListLayout;
};

// ============================================================================
// MessageDialog: 自定义提示框 (用于授权失败等场景)
// ============================================================================
class MessageDialog : public FramelessDialog {
    Q_OBJECT
public:
    explicit MessageDialog(const QString& msg, QWidget* parent = nullptr) 
        : FramelessDialog("系统提示", parent) 
    {
        setFixedSize(320, 180);
        m_stateBtn->hide(); 
        m_sidebarBtn->hide();
        
        auto* layout = new QVBoxLayout(m_contentArea);
        layout->setContentsMargins(20, 20, 20, 20);
        layout->setSpacing(20);

        auto* label = new QLabel(msg);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet("color: #BBB; font-size: 14px; line-height: 1.5;");
        label->setWordWrap(true);
        layout->addWidget(label);

        auto* btn = new QPushButton("确定");
        btn->setFixedSize(100, 32);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(
            "QPushButton { background-color: #333; color: #BBB; border: 1px solid #444; border-radius: 4px; font-weight: bold; } "
            "QPushButton:hover { background-color: #444; border: 1px solid #FF8C00; color: #FF8C00; }"
        );
        connect(btn, &QPushButton::clicked, this, &QDialog::accept);
        
        auto* btnLayout = new QHBoxLayout();
        btnLayout->addStretch();
        btnLayout->addWidget(btn);
        btnLayout->addStretch();
        layout->addLayout(btnLayout);
    }
};

#pragma pack(push, 1)
typedef struct _T_ATA_ID {
    unsigned short R1[10];
    unsigned short SN[10];
    unsigned short R2[235];
} T_ATA_ID;
#pragma pack(pop)

QString getDiskSerial(const QString& drivePath) {
    QString drive = drivePath.trimmed();
    if (drive.contains(":")) drive = drive.left(2);
    QString vol = "\\\\.\\" + drive;
    HANDLE hV = CreateFileW((LPCWSTR)vol.utf16(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hV == INVALID_HANDLE_VALUE) return "";
    STORAGE_DEVICE_NUMBER sdn; DWORD br = 0;
    if (!DeviceIoControl(hV, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn, sizeof(sdn), &br, NULL)) { CloseHandle(hV); return ""; }
    CloseHandle(hV);
    QString phys = QString("\\\\.\\PhysicalDrive%1").arg(sdn.DeviceNumber);
    HANDLE hD = CreateFileW((LPCWSTR)phys.utf16(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hD == INVALID_HANDLE_VALUE) return "";
    QString res;
    // 1. NVMe
    struct { STORAGE_PROPERTY_QUERY Q; STORAGE_PROTOCOL_SPECIFIC_DATA P; BYTE B[4096]; } nReq = {};
    nReq.Q.PropertyId = (STORAGE_PROPERTY_ID)51; nReq.Q.QueryType = PropertyStandardQuery;
    nReq.P.ProtocolType = (STORAGE_PROTOCOL_TYPE)17; nReq.P.DataType = 0; nReq.P.ProtocolDataRequestValue = 1;
    nReq.P.ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA); nReq.P.ProtocolDataLength = 4096;
    if (DeviceIoControl(hD, IOCTL_STORAGE_QUERY_PROPERTY, &nReq, sizeof(nReq), &nReq, sizeof(nReq), &br, NULL)) {
        res = QString::fromLatin1((const char*)nReq.B + 4, 20).trimmed();
    }
    // 2. SATA
    if (res.isEmpty()) {
        SENDCMDINPARAMS sci = {0}; sci.irDriveRegs.bCommandReg = 0xEC;
        struct { SENDCMDOUTPARAMS out; BYTE b[512]; } sOut = {0};
        if (DeviceIoControl(hD, SMART_RCV_DRIVE_DATA, &sci, sizeof(sci), &sOut, sizeof(sOut), &br, NULL)) {
            T_ATA_ID* p = (T_ATA_ID*)sOut.b; QByteArray ba;
            for (int i = 0; i < 10; ++i) { ba.append((char)(p->SN[i] >> 8)); ba.append((char)(p->SN[i] & 0xFF)); }
            res = QString::fromLatin1(ba).trimmed();
        }
    }
    CloseHandle(hD); return res;
}

int main(int argc, char *argv[]) {
    // [强制] 显式初始化 Qt 资源表
    Q_INIT_RESOURCE(resources);

    QApplication a(argc, argv);
    
    // 全局样式统筹：美化 QToolTip
    a.setStyleSheet(
        "QToolTip {"
        "  background-color: #2D2D2D;"
        "  color: #BBB;"
        "  border: 1px solid #444;"
        "  border-radius: 2px;"
        "  padding: 4px;"
        "}"
    );

    // [硬件序列号校验] 废除试用期，改用硬绑定
    QStringList whitelist = {
        "BFEBFBFF000306C3", "SGH412RF00", "494000PA0D9L", 
        "PHYS825203NX480BGN", "NA5360WJ", "NA7G89GQ", "03000210052122072519"
    };

    auto getWmi = [](const QString& arg) -> QString {
        QProcess p; p.start("wmic", arg.split(" "));
        if (p.waitForFinished(3000)) {
            QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
            QStringList lines = out.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);
            if (lines.size() >= 2) return lines[1].trimmed();
        }
        return "";
    };

    QStringList collected;
    collected << getWmi("cpu get processorid");
    collected << getWmi("baseboard get serialnumber");
    collected << getWmi("bios get serialnumber");
    collected << getDiskSerial("C:");
    QString appPath = QCoreApplication::applicationDirPath();
    if (appPath.contains(":")) collected << getDiskSerial(appPath.left(2));

    bool pass = false;
    for (const QString& id : collected) {
        if (!id.isEmpty() && whitelist.contains(id, Qt::CaseInsensitive)) { pass = true; break; }
    }

    if (!pass) {
        MessageDialog msgBox("请勿非法使用\n“请联系开发者 Telegram：SYQ_14”");
        msgBox.exec();
        return 0;
    }

    qRegisterMetaType<HotkeyConfig>("HotkeyConfig");
    KeyboardHook::instance().start();
    TimePasteWindow w; w.show();
    int r = a.exec();
    KeyboardHook::instance().stop();
    return r;
}

#include "main.moc"
