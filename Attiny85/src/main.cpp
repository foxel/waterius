#include "Setup.h"

#include <avr/pgmspace.h>
#include <USIWire.h>

#include "Power.h"
#include "SlaveI2C.h"
#include "Storage.h"
#include "counter.h"
#include <avr/wdt.h>
#include <avr/sleep.h>
#include <avr/power.h>  

// Для логирования раскомментируйте LOG_LEVEL_DEBUG в Setup.h
#ifdef DEBUG
	TinyDebugSerial mySerial;
#endif

/*
Версии прошивок 
FIRMWARE_VER

7 - 2019.03.01 - dontsovcmc
	1. Обновил фреймворк до Platformio Atmel AVR 1.12.5
	2. Время аварийного отключения ESP 120сек. 
	   Даже при отсутствии связи ESP раньше в таймауты уйдет и пришлет "спим".
	
*/

#define INPUT0_PIN  4          //Вход 1, Blynk: V0, горячая вода
#define INPUT1_PIN  3          //Вход 2, Blynk: V1, холодная вода (или лог)

#define FIRMWARE_VER   7   	   // Версия прошивки. Передается в ESP и на сервер в данных.

#define ESP_POWER_PIN    1     // пин включения ESP8266. 
#define BUTTON_PIN       2     // пин кнопки: (на линии SCL)
                               // Долгое нажатие: ESP включает точку доступа с веб сервером для настройки
							   // Короткое: ESP передает показания
#define LONG_PRESS_MSEC  3000  // время долгого нажатия кнопки, милисекунд  
                               

// Счетчики импульсов
static Counter counter(INPUT0_PIN);
static Counter counter2(INPUT1_PIN);

// Класс для подачи питания на ESP и нажатия кнопки
static ESPPowerPin esp(ESP_POWER_PIN);

// Данные
struct Header info = {FIRMWARE_VER, 0, 0, 0, 0, {0, 0} };

//Кольцевой буфер для хранения показаний на случай замены питания или перезагрузки
//Кольцовой нужен для того, чтобы превысить лимит записи памяти в 100 000 раз
//Записывается каждый импульс, поэтому для 10л/импульс срок службы памяти 10 000м3
//100к * 20 = 2 млн * 10 л / 2 счетчика = 10 000 000 л или 10 000 м3
static EEPROMStorage<Data> storage(20); // 8 byte * 20 + crc * 20

SlaveI2C slaveI2C;

volatile int wdt_count; // таймер может быть < 0 ?

/* Вектор прерываний сторожевого таймера watchdog */
ISR( WDT_vect ) { 
	wdt_count--;
}  

/* Подготовка сторожевого таймера watchdog */
void resetWatchdog() {
	
	MCUSR = 0; // очищаем все флаги прерываний
	WDTCR = bit( WDCE ) | bit( WDE ); // allow changes, disable reset, clear existing interrupt

#ifdef TEST_WATERIUS
	WDTCR = bit( WDIE ) | bit( WDP0 );      // 32 ms
	#define ONE_MINUTE 20  // ускоримся для теста
#else

	//WDTCR = bit( WDIE );                  // 16 ms
	//#define ONE_MINUTE ?

	// настраиваем период сна и кол-во просыпаний за 1 минуту
	// Итак, пробуждаемся (проверяем входы) каждые 128 мс
	// 1 минута примерно равна 480 пробуждениям
	WDTCR = bit( WDIE ) | bit( WDP0 ) | bit( WDP1 );     // 128 ms
	#define ONE_MINUTE 480

	//WDTCR = bit( WDIE ) | bit( WDP2 );                 // 250 ms
	//#define ONE_MINUTE 240

	//WDTCR = bit( WDIE ) | bit( WDP2 ) | bit( WDP0 );    // 500 ms
	//#define ONE_MINUTE 120
#endif
									
	wdt_reset(); // pat the dog
} 

// Проверяем входы на замыкание. 
// Замыкание засчитывается только при повторной проверке.
inline void counting() {

	if (counter.check_close(info.data.value0)) {
		storage.add(info.data);
	}
	#ifndef DEBUG
		if (counter2.check_close(info.data.value1)) {
			storage.add(info.data);
		}
	#endif
}

// Настройка. Вызывается однократно при запуске.
void setup() {

	info.service = MCUSR; //причина перезагрузки

	noInterrupts();
	ACSR |= bit( ACD ); //выключаем компаратор
	interrupts();
	resetWatchdog(); 
	adc_disable(); //выключаем ADC

	pinMode(BUTTON_PIN, INPUT); //кнопка на корпусе

	if (storage.get(info.data)) { //не первая загрузка
		info.resets = EEPROM.read(storage.size());
		info.resets++;
		EEPROM.write(storage.size(), info.resets);
	} else {
		EEPROM.write(storage.size(), 0);
	}

	DEBUG_CONNECT(9600); 
	LOG_DEBUG(F("==== START ===="));
	LOG_DEBUG(F("MCUSR"));
	LOG_DEBUG(info.service);
	LOG_DEBUG(F("RESET"));
	LOG_DEBUG(info.resets);
	LOG_INFO(F("Data:"));
	LOG_INFO(info.data.value0);
	LOG_INFO(info.data.value1);
}

// Проверка нажатия кнопки 
bool button_pressed() {

	if (digitalRead(BUTTON_PIN) == LOW)
	{	//защита от дребезга
		delayMicroseconds(20000);  //нельзя delay, т.к. power_off
		return digitalRead(BUTTON_PIN) == LOW;
	}
	return false;
}

// Замеряем сколько времени нажата кнопка в мс
unsigned long wait_button_release() {

	unsigned long press_time = millis();
	while(button_pressed())
		;  
	return millis() - press_time;
}


// Главный цикл, повторящийся раз в сутки или при настройке вотериуса
void loop() {
	
	// Отключаем все лишнее: ADC, Timer 0 and 1, serial interface
	power_all_disable();  

	// Режим сна
	set_sleep_mode( SLEEP_MODE_PWR_DOWN );

	// Настраиваем служебный таймер (watchdog)
	resetWatchdog(); 

	// Цикл опроса входов
	// Выход по прошествию WAKE_EVERY_MIN минут или по нажатию кнопки
	for (unsigned int i = 0; i < ONE_MINUTE && !button_pressed(); ++i)  {
		wdt_count = WAKE_EVERY_MIN; 
		while ( wdt_count > 0 ) {
			noInterrupts();

			counting(); //Опрос входов

			if (button_pressed()) { 
				interrupts();  // Пользователь нажал кнопку
				break;
			} else 	{
				interrupts();
				sleep_mode();  // Спим (WDTCR)
			}
		}
	}
		
	wdt_disable();        // disable watchdog
	power_all_enable();   // power everything back on

	info.voltage = readVcc();   // Текущее напряжение
	storage.get(info.data);     // Берем из хранилища текущие значения импульсов
	
	DEBUG_CONNECT(9600);
	LOG_INFO(F("Data:"));
	LOG_INFO(info.data.value0);
	LOG_INFO(info.data.value1);

	// Если пользователь нажал кнопку SETUP, ждем когда отпустит 
	// иначе ESP запустится в режиме программирования (да-да кнопка на i2c и 2 пине ESP)
	// Если кнопка не нажата или нажата коротко - передаем показания 
	unsigned long wake_up_limit;
	if (wait_button_release() > LONG_PRESS_MSEC) {

		LOG_DEBUG(F("SETUP pressed"));
		slaveI2C.begin(SETUP_MODE);	
		wake_up_limit = SETUP_TIME_MSEC; //5 мин при настройке
	} else {

		LOG_DEBUG(F("wake up for transmitting"));
		slaveI2C.begin(TRANSMIT_MODE);
		wake_up_limit = WAIT_ESP_MSEC; //15 секунд при передаче данных
	}

	esp.power(true);
	LOG_DEBUG(F("ESP turn on"));
	
	while (!slaveI2C.masterGoingToSleep() && !esp.elapsed(wake_up_limit)) {

		counting();
		delayMicroseconds(65000);
		if (wait_button_release() > LONG_PRESS_MSEC) {
			break; // принудительно выключаем
		}
	}

	esp.power(false);
	slaveI2C.end();			// выключаем i2c slave.

	if (!slaveI2C.masterGoingToSleep()) {
		LOG_ERROR(F("ESP wake up fail"));
	}
}
