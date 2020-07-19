#include "ESmart.hpp"

/* 
    Loading configs and local data
 */
void setup() {
    Serial.begin(115200);
    if (loadConfigs()) {
        connect();
        delay(200);
        begin();
    }
}

/* 
    handling buttons and alarms
*/
void loop() {
    for (size_t i = 0; i < buttons.size(); i++) {
        buttons[i].tick();
    }
    if (isInternetConnected()) Alarm.delay(0);
}

/* 
    Connecting to WiFi
 */
void connect() {
    WiFi.begin(configs.wifiAp, configs.wifiPass);
    WiFi.setAutoConnect(true);

    INFO("Connecting to: %s\n", configs.wifiAp.c_str());

    int wifiTimout = 0;

    while (wifiTimout <= WIFI_TIMEOUT) {
        if (WiFi.status() == WL_CONNECTED) break;
        delay(200);
        wifiTimout++;
        INFO("Retrying WiFi connection: %d/%d\n\n", wifiTimout, WIFI_TIMEOUT);
    }

    INFOM("Connected");
}

/*
    Begin time syncing a initializing firebase and firebase stream
*/
void begin() {
    INFOM("Start time syncing");
    timeClient.begin();
    int ntpTimeOut = 0;

    while (ntpTimeOut <= NTP_TIMEOUT) {
        if (timeClient.update()) {
            isConnected = true;
            INFOM("NTP client connected");
            break;
        } else {
            INFO("Retrying NTP connection: %d/%d\n\n", ntpTimeOut, NTP_TIMEOUT);
            isConnected = false;
        }
    }

    if (isInternetConnected()) {
        setTime(timeClient.getEpochTime());
        INFO("Done syncing, current time: %ld\n\n", timeClient.getEpochTime());

        Firebase.begin(configs.firebaseUrl, configs.firebaseKey);

        Firebase.setMaxRetry(firebaseJobData, 5);
        Firebase.setMaxErrorQueue(firebaseJobData, 10);
        Firebase.setMaxRetry(firebaseStreamData, 5);
        Firebase.setMaxErrorQueue(firebaseStreamData, 10);

        firebaseJobData.setResponseSize(1024);
        firebaseJobData.setBSSLBufferSize(1024, 1024);

        firebaseStreamData.setResponseSize(2048);
        firebaseStreamData.setBSSLBufferSize(1024, 1024);

        Firebase.beginStream(firebaseStreamData, configs.getUserPath());
        Firebase.setStreamCallback(firebaseStreamData, streamCallback);
    } else {
        INFOM("Couldn't connect to internet working in offline mode");
    }
}

/* 
    Loading configs from file system using LittleFS and parsing json data using ArduinoJson
 */
bool loadConfigs() {
    INFOM("Loading configs");

    if (!beginWrite()) return false;

    File configFile = LittleFS.open("/config.json", "r");
    File localDataFile = LittleFS.open("/data.json", "r");

    if (!configFile) {
        INFOM("Couldn't open config file");
        return false;
    }

    if (!localDataFile) {
        INFOM("Couldn't open data file");
    }

    DynamicJsonDocument configDoc(250);
    DynamicJsonDocument localData(2048);

    auto error1 = deserializeJson(configDoc, configFile);
    auto error2 = deserializeJson(localData, localDataFile);

    if (error1) {
        INFOM("Failed to deserialize config file");
        return false;
    }

    if (error2) {
        INFOM("Failed to deserialize data file");
    }

    configs = Configs(configDoc);
    initLocalData(localData);

    configFile.close();
    localDataFile.close();

    INFOM("Config loaded successfuly");

    endWrite();

    configDoc.clear();
    configDoc.garbageCollect();

    localData.clear();
    localData.garbageCollect();

    return true;
}

/* 
    Handeling Firebase stream callback
 */
void streamCallback(StreamData data) {
    if (data.dataType() == JSON) {
        INFO("Data received: %s\n\n", data.jsonString().c_str());
        DynamicJsonDocument object(2048);
        deserializeJson(object, data.jsonString());
        handleReceivedData(object);

        object.clear();
        object.garbageCollect();
    }
}

void handleReceivedData(DynamicJsonDocument &document) {
    if (!document["id"].isNull()) {
        EsmartFirebase esmartFirebase;
        esmartFirebase.init(document);

        INFO("Handeling server data: %s\n\n", esmartFirebase.toString().c_str());

        writePin(esmartFirebase.pin, esmartFirebase.ledPin, esmartFirebase.state);

        esmartFirebase.relayState = readPin(esmartFirebase.pin);

        updateNode(esmartFirebase);
        createAlarms(esmartFirebase);

    } else if (document["relayState"].isNull()) {
        JsonObject obj = document.as<JsonObject>();
        for (auto kv : obj) {
            EsmartFirebase esmartFirebase;
            esmartFirebase.init(kv.value());

            INFO("Handeling initial server data: %s\n\n", esmartFirebase.toString().c_str());
            INFO("Pin state: %d\n\n", readPin(esmartFirebase.pin));

            if (esmartFirebase.defaultState == -1 && esmartFirebase.relayState != readPin(esmartFirebase.pin)) {
                writePin(esmartFirebase.pin, esmartFirebase.ledPin, esmartFirebase.state);

                esmartFirebase.relayState = readPin(esmartFirebase.pin);

                updateNode(esmartFirebase);
            } else if (esmartFirebase.defaultState != -1 && esmartFirebase.defaultState != esmartFirebase.relayState) {
                writePin(esmartFirebase.pin, esmartFirebase.ledPin, esmartFirebase.defaultState);

                esmartFirebase.relayState = readPin(esmartFirebase.pin);
                esmartFirebase.state = readPin(esmartFirebase.pin);

                updateNode(esmartFirebase);
            } else {
                setLocalData(esmartFirebase);
            }

            createAlarms(esmartFirebase);
        }
    }
}

void initLocalData(DynamicJsonDocument &document) {
    JsonObject obj = document.as<JsonObject>();
    for (auto kv : obj) {
        EsmartFirebase esmartFirebase;
        esmartFirebase.init(kv.value());

        INFO("Initiatinng initial data: %s\n\n", esmartFirebase.toString().c_str());

        if (esmartFirebase.defaultState == -1) {
            writePin(esmartFirebase.pin, esmartFirebase.ledPin, esmartFirebase.state);
            pinMode(esmartFirebase.pin, OUTPUT);
            pinMode(esmartFirebase.ledPin, OUTPUT);
        } else if (esmartFirebase.defaultState != -1) {
            writePin(esmartFirebase.pin, esmartFirebase.ledPin, esmartFirebase.defaultState);
            pinMode(esmartFirebase.pin, OUTPUT);
            pinMode(esmartFirebase.ledPin, OUTPUT);
        }

        createButton(esmartFirebase);
    }
}

void setLocalData(EsmartFirebase &esmart) {
    if (beginWrite()) {
        INFO("Setting local data: %s\n\n", esmart.toString().c_str());

        File localDataFile = LittleFS.open("/data.json", "r+");
        if (!localDataFile) INFOM("Failed to open data file");

        DynamicJsonDocument document(2048);

        deserializeJson(document, localDataFile);
        localDataFile = LittleFS.open("/data.json", "w+");

        document[esmart.id] = esmart.getJsonDoc();

        serializeJson(document, localDataFile);

        localDataFile.close();
        document.clear();
        document.garbageCollect();

        endWrite();
    }
};

void updateNode(EsmartFirebase &esmart) {
    INFO("Updating node data: %s\n\n", esmart.toString().c_str());

    if (isInternetConnected()) {
        delay(250);
        FirebaseJson json = esmart.getFirebaseJson();
        Firebase.updateNode(firebaseJobData, configs.getUserPath(esmart.id), json);
    }

    delay(250);
    setLocalData(esmart);
}

void doWork(FutureJob &work) {
    INFO("Doing local work: %s\n\n", work.esmart.toString().c_str());

    writePin(work.esmart.pin, work.esmart.ledPin, work.esmart.state);

    updateNode(work.esmart);
}

void createButton(EsmartFirebase &esmart) {
    INFO("Creating button: %s\n\n", esmart.toString().c_str());

    OneButton button(esmart.buttonPin, esmart.buttonState, FutureJob(esmart));

    button.attachClick([&](FutureJob &work) {
        INFO("Triggering on click: %s\n\n", work.esmart.toString().c_str());

        work.esmart.state = !readPin(work.esmart.pin);
        work.esmart.relayState = work.esmart.state;

        doWork(work);
    });
    button.attachLongPressStop([&](FutureJob &work) {
        INFO("Triggering on long press stop: %s\n\n", work.esmart.toString().c_str());

        work.esmart.state = !readPin(work.esmart.pin);
        work.esmart.relayState = work.esmart.state;

        // doWork(work);

        longPressReset = 0;
    });
    button.attachDuringLongPress([&](FutureJob &work) {
        INFO("Triggering on long press: %s\n\n", work.esmart.toString().c_str());

        if (longPressReset == 0) {
            longPressReset = millis();
        } else if (millis() - longPressReset > 5000) {
            ESP.reset();
        }
    });
    buttons.push_back(button);
}

void createAlarms(EsmartFirebase &esmart) {
    createOffAlarm(esmart);
    createOnAlarm(esmart);
}

void createOffAlarm(EsmartFirebase &esmart) {
    time_t offTime = static_cast<time_t>(esmart.endTime);

    if (Alarm.isAllocated(esmart.pin + 1)) {
        if (offTime != 0) {
            INFO("Updating off alarm: %s\n\n", esmart.toString().c_str());

            tmElements_t element;
            breakTime(offTime, element);
            Alarm.write(esmart.pin + 1, AlarmHMS(element.Hour, element.Minute, element.Second));
        } else {
            INFO("Deleting off alarm: %s\n\n", esmart.toString().c_str());

            Alarm.free(esmart.pin + 1);
        }
    } else if (offTime > 0) {
        tmElements_t element;
        breakTime(offTime, element);
        FutureJob work = FutureJob(esmart);
        INFO("Creating off alarm: %s\n\n", esmart.toString().c_str());

        Alarm.alarmRepeat(element.Hour, element.Minute, element.Second, esmart.pin + 1, work, [&](FutureJob work) {
            INFO("Triggering off alarm: %s\n\n", esmart.toString().c_str());

            work.esmart.state = !readPin(work.esmart.pin);
            work.esmart.relayState = !readPin(work.esmart.pin);
            doWork(work);
        });
    }
}

void createOnAlarm(EsmartFirebase &esmart) {
    time_t onTime = static_cast<time_t>(esmart.startTime);

    if (Alarm.isAllocated(esmart.pin)) {
        if (onTime != 0) {
            INFO("Updating on alarm: %s\n\n", esmart.toString().c_str());
            tmElements_t element;
            breakTime(onTime, element);
            Alarm.write(esmart.pin, AlarmHMS(element.Hour, element.Minute, element.Second));
        } else {
            INFO("Deleting on alarm: %s\n\n", esmart.toString().c_str());
            Alarm.free(esmart.pin);
        }
    } else if (onTime > 0) {
        INFO("Creating on alarm: %s\n\n", esmart.toString().c_str());

        tmElements_t element;
        breakTime(onTime, element);
        FutureJob work = FutureJob(esmart);

        Alarm.alarmRepeat(element.Hour, element.Minute, element.Second, esmart.pin, work, [&](FutureJob work) {
            INFO("Triggering on alarm: %s\n\n", esmart.toString().c_str());

            work.esmart.state = !readPin(work.esmart.pin);
            work.esmart.relayState = !readPin(work.esmart.pin);
            doWork(work);
        });
    }
}

int readPin(int pin) {
    int val = digitalRead(pin) ^ READ_OPERATOR;
    INFO("Reading pin %d value %d and new val %d\n\n", pin, digitalRead(pin), val);
    return val;
}

void writePin(int pin, int statusPin, int val) {
    int newVal = val ^ WRITE_OPERATOR;
    INFO("Writing to pin %d new value %d and val %d\n\n", pin, val, newVal);
    digitalWrite(pin, newVal);
    digitalWrite(statusPin, val);
}