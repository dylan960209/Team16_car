// ==========================================
// 1. 藍牙與 RemoteXY 相關設定 (ESP32 BLE)
// ==========================================
#define REMOTEXY_MODE__ESP32CORE_BLE
#include <BLEDevice.h>

// 這裡可以改成你們專屬的藍牙名稱，方便現場配對
#define REMOTEXY_BLUETOOTH_NAME "Team_EBS" 

#include <RemoteXY.h>

#pragma pack(push, 1)  
uint8_t const PROGMEM RemoteXY_CONF_PROGMEM[] = 
  { 255,1,0,0,0,30,0,19,0,0,0,0,31,1,106,200,1,1,1,0,
  2,32,91,44,22,0,2,26,31,31,79,78,0,79,70,70,0 };
  
struct {
  uint8_t switch_01;     // =1 觸發 EBS, =0 正常狀態
  uint8_t connect_flag;  // =1 藍牙已連線, =0 未連線
} RemoteXY;   
#pragma pack(pop)

#include <Arduino.h>

// --- 馬達腳位定義 ---
const int ENA = 25;
const int IN1 = 26;
const int IN2 = 33;

const int ENB = 13;
const int IN3 = 14;
const int IN4 = 27;

// --- 5路循跡感測器腳位定義 ---
const int S1 = 16;  // 最左
const int S2 = 17;  // 左
const int S3 = 18;  // 中
const int S4 = 19;  // 右
const int S5 = 21;  // 最右

// --- ASL 腳位定義 ---
const int PIN_RED = 32;
const int PIN_GREEN = 5;
const int PIN_BLUE = 23;

// --- VLS 與狀態定義 ---
const int PIN_VLS = 0;
bool isRunning = false; 

// --- 🎯 最簡版循跡全域變數 ---
float error = 0;
float Kp = 32.0;       // 🎯 P控制轉彎力道
int baseSpeed = 240;   // 🌟 提高基礎速度到 100，跨過 L298N 的壓降門檻

// --- 函式宣告 ---
void setASLColor(String color);
void moveMotors(int left, int right);

void setup() {
  RemoteXY_Init();  // 初始化藍牙與 RemoteXY
  Serial.begin(115200);
  
  // 馬達腳位全部恢復標準輸出
  pinMode(ENA, OUTPUT); pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(ENB, OUTPUT); pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  
  pinMode(S1, INPUT); pinMode(S2, INPUT); pinMode(S3, INPUT); pinMode(S4, INPUT); pinMode(S5, INPUT);
  pinMode(PIN_RED, OUTPUT); pinMode(PIN_GREEN, OUTPUT); pinMode(PIN_BLUE, OUTPUT);
  pinMode(PIN_VLS, INPUT_PULLUP);

  setASLColor("BLUE");
  Serial.println("系統待命，請按下 BOOT 按鈕起跑...");
}

void loop() {
  RemoteXY_Handler(); // 保持藍牙連線與資料更新
  
  // ----------------------------------------------------
  // 🌟 最高優先級：EBS 緊急制動監聽 
  // ----------------------------------------------------
  if (RemoteXY.switch_01 != 0) { 
      moveMotors(0, 0);       // 強制馬達扭矩歸零
      isRunning = false;      // 剝奪自主導航權限
      setASLColor("GREEN");   // 切換為安全狀態 (綠燈恆亮) 
      Serial.println("🚨 EBS 觸發！車輛已鎖死進入安全狀態！");
      return;                 // 強制跳出 loop
  }
  
  // ==========================================
  // 1. 待命狀態：等待 VLS 觸發
  // ==========================================
  if (isRunning == false) {
    if (digitalRead(PIN_VLS) == LOW) {
      RemoteXY_delay(50); // 防彈跳
      if (digitalRead(PIN_VLS) == LOW) { 
        Serial.println("VLS 觸發！進入自主導航模式！");
        setASLColor("RED");   // 進入紅燈 (自主導航狀態)
        RemoteXY_delay(1000); // 安全延遲
        isRunning = true; 
      }
    }
  } 
  
  // ==========================================
  // 2. 自主導航狀態：執行最簡純 P 循跡 + 智慧定向倒退搜尋
  // ==========================================
  else if (isRunning == true) {
    int val1 = !digitalRead(S1); 
    int val2 = !digitalRead(S2); 
    int val3 = !digitalRead(S3);
    int val4 = !digitalRead(S4); 
    int val5 = !digitalRead(S5);
    
    Serial.print("感測器狀態 (左至右): [ ");
    Serial.print(val1); Serial.print(" | ");
    Serial.print(val2); Serial.print(" | ");
    Serial.print(val3); Serial.print(" | ");
    Serial.print(val4); Serial.print(" | ");
    Serial.print(val5); Serial.println(" ]");

    // 🎯 情況 A：有任何一個感測器踩到黑線 (正常循跡)
    if (val1 == 1 || val2 == 1 || val3 == 1 || val4 == 1 || val5 == 1) {
      
      // 更新循跡誤差 (當全為 0 時不會進來這裡，所以 error 會保持最後一次的值)
      error = (-2 * val1) + (-1 * val2) + (0 * val3) + (1 * val4) + (2 * val5);

      float correction = Kp * error;

      int leftSpeed  = baseSpeed + correction;
      int rightSpeed = baseSpeed - correction;

      leftSpeed  = constrain(leftSpeed, 0, 255);
      rightSpeed = constrain(rightSpeed, 0, 255);

      moveMotors(leftSpeed, rightSpeed);
    } 
    // 🌟 情況 B：當 val1 到 val5 全為 0 時 (智慧定向倒退搜尋)
    else {
      Serial.println("⚠️ 遺失黑線！執行原地甩尾搜尋中...");
      
      // 考量 L298N 壓降與輪胎靜摩擦力，設定一個強大的原地旋轉動力
      // 建議設定在 120 到 150 之間，數字越大甩得越暴力
      int spinSpeed = 140; 
      
      if (error > 0) {
        // 最後線在右邊 (例如 S4 或 S5 有訊號) -> 車身需要往右甩
        // 左輪往前衝，右輪往後退
        moveMotors(spinSpeed, -spinSpeed);
      } 
      else if (error < 0) {
        // 最後線在左邊 (例如 S1 或 S2 有訊號) -> 車身需要往左甩
        // 左輪往後退，右輪往前衝
        moveMotors(-spinSpeed, spinSpeed);
      } 
      else {
        // 極端情況：如果最後是完美的 0 (直線正中衝出賽道)，預設向右盲搜
        moveMotors(spinSpeed, -spinSpeed);
      }
    }
  }
}


// --- 輔助函式 ---
void setASLColor(String color) {
  digitalWrite(PIN_RED, color == "RED" ? HIGH : LOW);
  digitalWrite(PIN_GREEN, color == "GREEN" ? HIGH : LOW);
  digitalWrite(PIN_BLUE, color == "BLUE" ? HIGH : LOW);
}

// 🌟 馬達驅動回歸最純粹、相容性最高的 analogWrite
void moveMotors(int left, int right) {
  digitalWrite(IN1, left >= 0 ? HIGH : LOW);
  digitalWrite(IN2, left >= 0 ? LOW : HIGH);
  analogWrite(ENA, abs(left));
  
  digitalWrite(IN3, right >= 0 ? HIGH : LOW);
  digitalWrite(IN4, right >= 0 ? LOW : HIGH);
  analogWrite(ENB, abs(right));
}