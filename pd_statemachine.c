#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// ==========================================
// 1. 全域硬體規格邊界與位元遮罩定義 (Global Hardware Constraints & Masks)
// ==========================================
#define MAX_HARDWARE_PORTS    4   // 晶片實體腳位上限 (由實體 ADC 通道/GPIO 決定)
#define BIT_VBUS_EN          (1 << 0)  // VBUS 供電開關控制位元 (1=開啟供電, 0=切斷/安全隔離)
#define BIT_DISCH_EN         (1 << 1)  // VBUS 殘留電壓放電迴路 (1=開啟放電, 0=關閉放電)

// 【編譯期防禦】：若未來工程師修改客製化巨集超出硬體物理極限，編譯時直接攔截阻斷
_Static_assert(MAX_HARDWARE_PORTS <= 4, "SYSTEM ERROR: This IC chip only contains 4 physical CC pins!");

// ==========================================
// 2. 狀態機狀態與事件定義 (State Machine Enumeration)
// ==========================================
typedef enum {
    STATE_POR_SELF_TEST,    // 晶片上電自檢 (Power-On Reset & Safety Check)
    STATE_CC_DETECTION,     // CC 腳位偵測 (Rp/Rd 分壓確立主從角色)
    STATE_SRC_CAPS,         // 發送 BMC 協定宣告 (Source Capabilities)
    STATE_REQUEST,          // 接收 Request 並準備電壓跳變 (Fail-Safe 斷閘)
    STATE_PS_RDY,           // 供電放行審查 (非阻塞型電壓爬坡追蹤)
    STATE_ERROR_HANDLING,   // 容錯防護隔離 (開啟放電、杜絕擊穿)
} PD_State_t;

// ==========================================
// 3. 【多口並聯核心】：用結構體封裝每個接口獨立的記憶體空間
// ==========================================
typedef struct {
    uint8_t       id;              // 實體接口編號 (Port 0, Port 1...)
    PD_State_t    current_state;   // 該接口特有的獨立狀態紀錄 (取代 Static / 杜絕全域變數)
    uint8_t       pd_register;     // 該接口專屬的硬體模擬暫存器 (位元手術操作對象)
    bool          hardware_stable; // 實體層硬體電源動態狀態 (VBUS 爬坡回授)
    uint32_t      timeout_counter; // 各埠獨立的非阻塞型計時器 (Timeout Protection)
} USB_PD_Port_t;

// ==========================================
// 4. FAE 除錯核心引擎：透過指標 (*) 傳址，實現非阻塞多路調度
// ==========================================
void run_multi_port_pd_statemachine(USB_PD_Port_t *port) {
    // 使用箭頭運算子 (->) 透過指標精準存取該 Port 的獨立記憶體空間
    switch (port->current_state) {
        
        case STATE_POR_SELF_TEST:
            printf("[Port %d][POR_SELF_TEST] 啟動 Power-On Reset。驗證內部基準電壓與 OCP 防護...\n", port->id);
            // 執行硬體防禦自檢：利用位元與 (&) 遮罩確認 VBUS 腳位無預期初始短路
            if ((port->pd_register & BIT_VBUS_EN) == 0) {
                printf("  -> [Port %d Check] 硬體自檢通過，未偵測到初期短路。進入連線偵測。\n", port->id);
                port->current_state = STATE_CC_DETECTION;
            } else {
                printf("  ->  [Port %d FATAL] 初始狀態異常！暫存器 VBUS 已開啟，強制隔離。\n", port->id);
                port->current_state = STATE_ERROR_HANDLING;
            }
            break;

        case STATE_CC_DETECTION:
            printf("[Port %d][CC_DETECTION] 偵測 CC 腳位分壓，確立主從角色。\n", port->id);
            port->current_state = STATE_SRC_CAPS; 
            break;

        case STATE_SRC_CAPS:
            printf("[Port %d][SRC_CAPS] 發送 BMC 協定宣告 (Source Capabilities)。\n", port->id);
            port->current_state = STATE_REQUEST;
            break;

        case STATE_REQUEST:
            printf("[Port %d][REQUEST] 收到 Request，準備執行電壓跳變。\n", port->id);
            
            // 【硬體防禦位元手術】：精準清除 BIT_VBUS_EN，安全斷閘隔離，防範跳變期 Overshoot
            port->pd_register &= ~BIT_VBUS_EN; 
            printf("  -> [Port %d Fail-Safe] 暫存器清零，關閉供電閘，等待 20V 電壓爬坡。\n", port->id);
            
            port->timeout_counter = 0; // 重置該埠的超時計數器 (動態防禦鏈起點)
            port->current_state = STATE_PS_RDY;
            break;

        case STATE_PS_RDY:
            // 【非阻塞型設計】：用反向 if 搭配 break 取代 blocking while，快進快出，讓出 CPU 主權
            if (!port->hardware_stable) {
                port->timeout_counter++;
                printf("[Port %d][PS_RDY] VBUS 20V 爬坡中... (目前系統追蹤計數: %d/3)\n", port->id, port->timeout_counter);
                
                // 超時防呆機制 (Timeout Protection)
                if (port->timeout_counter >= 3) { 
                    printf("  ->  [Port %d ERROR] VBUS 爬坡超時或電壓下陷(Sag)！觸發硬體防護。\n", port->id);
                    port->current_state = STATE_ERROR_HANDLING;
                    return; // 發生致命錯誤，即時止損，強制防火牆彈出，拒絕後續無效執行
                }
                // 硬體電壓尚未穩定，跳出 switch，交還執行權給系統迴圈去輪詢其他 Port
                break; 
            }

            // 【位元手術放行】：硬體穩定，精準將 BIT_VBUS_EN 置 1，開啟功率 MOSFET
            port->pd_register |= BIT_VBUS_EN;
            printf("  ->  [Port %d SUCCESS] VBUS 穩定！正式安全放行大功率供電。\n", port->id);
            break;

        case STATE_ERROR_HANDLING:
            // 實施硬體隔離，切斷供電，並強行開啟放電迴路 (BIT_DISCH_EN 置 1) 洩放殘留危險電壓
            port->pd_register &= ~BIT_VBUS_EN;
            port->pd_register |= BIT_DISCH_EN;
            printf("  ->  [Port %d ISOLATED] 已物理隔離故障端口，開啟放電迴路防止過衝(Overshoot)擊穿晶片。\n", port->id);
            break;
    }
}

// ==========================================
// 5. 主程式：動態序列化、測試案例驅動 (Test Case Driven)
// ==========================================
int main(void) {
    printf("==================================================================\n");
    printf("--- 瑞昱原廠級面試專案：Multi-Port 非阻塞多接口並聯狀態機模擬 ---\n");
    printf("==================================================================\n\n");

    // 【指定初始化器 (Designated Initializers)】：不按順序名入座，兼顧代碼彈性與視覺可讀性
    USB_PD_Port_t my_hub_ports[] = {
        // Port 0: 模擬正常相容設備。預期在第 5 週期外部電壓穩壓成功。
        { .id = 0, .current_state = STATE_POR_SELF_TEST, .pd_register = 0x00, .hardware_stable = false, .timeout_counter = 0 },
        
        // Port 1: 模擬硬體故障設備。電壓永遠無法穩定，預期觸發超時安全隔離。
        { .id = 1, .current_state = STATE_POR_SELF_TEST, .pd_register = 0x00, .hardware_stable = false, .timeout_counter = 0 }
    };

    // 編譯期自動計算配置的連接埠總數
    int total_ports = sizeof(my_hub_ports) / sizeof(my_hub_ports[0]);

    // 【軟硬體邊界攔截】：檢查配置總數是否超出晶片實體負載上限，實施初始期 Fail-Safe
    if (total_ports > MAX_HARDWARE_PORTS) {
        printf("[SYSTEM FATAL] 配置的 Port 數 (%d) 超過晶片硬體支援上限 (%d)！初始化失敗。\n", 
               total_ports, MAX_HARDWARE_PORTS);
        return -1;
    }

    // 模擬 7 個系統時間週期迴路 (Main System Clock Cycles)
    for (int cycle = 1; cycle <= 7; cycle++) {
        printf("===  系統主核心時間週期 (System Cycle %d) ===\n", cycle);
        
        for (int p = 0; p < total_ports; p++) {
            
            // 【實務動態環境模擬】：
            // 模擬 Port 0 在主核心走到第 5 個週期時，外部 MOSFET 與電容完成穩壓
            if (my_hub_ports[p].id == 0 && cycle == 5) {
                my_hub_ports[p].hardware_stable = true; 
            }
            // 註：Port 1 故意不給它 hardware_stable = true，模擬實體電路漏電、短路或 TVS 損壞

            // 呼叫核心引擎，傳入該 Port 的記憶體指標 (傳址呼叫，隔離記憶體)
            run_multi_port_pd_statemachine(&my_hub_ports[p]); 
        }
        
        printf("------------------------------------------------------------------\n");
    }

    printf("\n 【FAE 除錯報告總結】：\n");
    printf("-> Port 0 最終暫存器狀態: 0x%02X (預期為 0x01: BIT_VBUS_EN 成功導通)\n", my_hub_ports[0].pd_register);
    printf("-> Port 1 最終暫存器狀態: 0x%02X (預期為 0x02: BIT_DISCH_EN 強制放電安全隔離)\n", my_hub_ports[1].pd_register);
    
    return 0;
}