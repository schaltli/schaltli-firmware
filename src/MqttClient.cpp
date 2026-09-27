#include "MqttClient.h"
#include "DeviceInfo.h"

// Static instance pointer for callback forwarding
MqttClient* MqttClient::instance_ = nullptr;

MqttClient::MqttClient() 
  : mqttClient_(wifiClient_),
    port_(1883),
    lastReconnectAttempt_(0) {
  
  // Unique per board, and prefixed with this device's own type rather than
  // the M5 Dial's - the ported version hardcoded "M5Dial-", which made this
  // board announce itself under an M5 Dial client id and, since the client
  // id *is* the MQTT topic namespace (schaltli/<clientId>/...), put its
  // hello, deploy and status topics in the wrong place entirely. Caught the
  // first time this firmware connected to a real broker.
  //
  // DEVICE_ID rather than a fresh literal, so this cannot drift from the
  // device's own identity the way the hardcoded name did.
  clientId_ = String(DEVICE_ID) + "-" + String(ESP.getEfuseMac(), HEX);
  
  // Set static instance for callback forwarding
  instance_ = this;

  // Set PubSubClient callback
  mqttClient_.setCallback(staticCallback);

  // PubSubClient defaults to a 256-byte buffer for topic+payload+overhead
  // combined (MQTT_MAX_PACKET_SIZE) - too small for a realistic multi-field
  // JSON payload (a "json"-type Topic's message, see ProjectTypes.h's
  // MQTTTopic), which would otherwise just get silently truncated/dropped.
  // Must be set before connect() - PubSubClient allocates the buffer at
  // that size up front, so this belongs here at construction, not later.
  mqttClient_.setBufferSize(1024);

  // PubSubClient's default 15s keepalive (docs/device-contract.md's
  // shared broker also runs a dozen+ other MQTT clients, e.g. Home
  // Assistant's mqttthing integrations, all configured with a 60s
  // keepalive) made this device far more sensitive to any transient
  // broker/network hiccup than everything else on the same broker -
  // found 2026-08-10 as the real cause of a live-published MQTT message
  // (a deploy trigger, specifically) intermittently never reaching this
  // device at all: `mosquitto` logs showed it reconnecting every ~5-15s
  // fairly continuously, meaning it spent a meaningful fraction of time
  // in a disconnected gap where nothing could be delivered. A message
  // published *while already connected* could land in exactly that gap
  // and simply never arrive - the same trigger DOES eventually get
  // through if the device (or its connection) resets, since the trigger
  // is retained and gets redelivered on the next successful subscribe,
  // which is what made this look like "works after a manual reset,
  // silently drops otherwise" rather than a clean pass/fail. Matching the
  // other clients' 60s keepalive gives this device the same tolerance for
  // brief hiccups they already have.
  mqttClient_.setKeepAlive(60);
}

void MqttClient::configure(const String& host, uint16_t port, const String& username, const String& password) {
  host_ = host;
  port_ = port;
  username_ = username;
  password_ = password;
  
  mqttClient_.setServer(host_.c_str(), port_);
}

void MqttClient::setCallback(MessageCallback callback) {
  messageCallback_ = callback;
}

bool MqttClient::connect() {
  if (host_.isEmpty()) {
    return false;
  }
  
  if (!WiFi.isConnected()) {
    return false;
  }

  // Last Will and Testament: the broker publishes this (retained) on our
  // behalf the moment our TCP connection drops without a clean
  // disconnect - the only way a subscriber (the designer's deploy dialog)
  // can tell "was online, went away unexpectedly" apart from "still
  // connected" without waiting for a timeout of its own. We publish the
  // "online" counterpart ourselves right below, once connect() actually
  // succeeds.
  String statusTopic = String(TOPIC_PREFIX) + "/" + clientId_ + "/status";
  bool connected = mqttClient_.connect(
    clientId_.c_str(),
    username_.isEmpty() ? nullptr : username_.c_str(),
    username_.isEmpty() ? nullptr : password_.c_str(),
    statusTopic.c_str(),
    1,     // willQos
    true,  // willRetain
    "offline"
  );

  if (connected) {
    connectCount_++;
    mqttClient_.publish(statusTopic.c_str(), "online", true);

    // Re-subscribe to keys after connection
    if (!subscribedTopics_.empty()) {
      for (const String& key : subscribedTopics_) {
        mqttClient_.subscribe(key.c_str());
      }
    }

    if (connectedCallback_) connectedCallback_();
  }

  return connected;
}

bool MqttClient::isConnected() {
  return mqttClient_.connected();
}

bool MqttClient::subscribe(const String& key) {
  if (key.isEmpty()) {
    return false;
  }
  
  // Check if already subscribed
  for (const String& subscribedKey : subscribedTopics_) {
    if (subscribedKey == key) {
      if (!isConnected()) {
        return true;
      }
      return mqttClient_.subscribe(key.c_str());
    }
  }
  
  subscribedTopics_.push_back(key);

  if (!isConnected()) {
    return true;
  }

  return mqttClient_.subscribe(key.c_str());
}

void MqttClient::subscribeToKeys(const std::vector<String>& keys) {
  for (const String& key : keys) {
    subscribe(key);
  }
}

bool MqttClient::unsubscribe(const String& key) {
  if (!isConnected()) {
    return false;
  }
  
  bool success = mqttClient_.unsubscribe(key.c_str());
  
  if (success) {
    // Remove from subscribed list
    for (auto it = subscribedTopics_.begin(); it != subscribedTopics_.end(); ++it) {
      if (*it == key) {
        subscribedTopics_.erase(it);
        break;
      }
    }
  }
  
  return success;
}

bool MqttClient::publishAction(const String& actionName) {
  if (actionName.isEmpty()) {
    return false;
  }

  if (!isConnected()) {
    return false;
  }

  // Increment counter for this action (or initialize to 1 if new)
  actionCounters_[actionName]++;
  unsigned long counter = actionCounters_[actionName];

  // Convert counter to string payload
  String payload = String(counter);

  // Publish: topic = actionName, payload = counter value, retain = false
  bool success = mqttClient_.publish(actionName.c_str(), payload.c_str(), false);
  return success;
}

bool MqttClient::publish(const String& topic, const String& payload, bool retain) {
  if (topic.isEmpty() || !isConnected()) {
    return false;
  }

  return mqttClient_.publish(topic.c_str(), payload.c_str(), retain);
}

void MqttClient::clearSubscriptions() {
  for (const String& key : subscribedTopics_) {
    mqttClient_.unsubscribe(key.c_str());
  }
  subscribedTopics_.clear();
}

void MqttClient::loop() {
  if (!host_.isEmpty()) {
    // Try to reconnect if disconnected
    if (!mqttClient_.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt_ > reconnectBackoffMs_) {
        // Temporary diagnostic (2026-08-10): this device was found
        // reconnecting to the broker roughly every 5s continuously
        // (matching RECONNECT_INTERVAL_MS exactly), which explains why a
        // live-published MQTT message (e.g. a deploy trigger) so often
        // failed to reach it - it was almost never in a stable, long-lived
        // connected state. PubSubClient::state() reports why *it* thinks
        // the connection is down (MQTT_CONNECTION_TIMEOUT=-4 means a
        // PINGREQ went unanswered, MQTT_CONNECTION_LOST=-3 means the
        // underlying socket read failed, etc. - see PubSubClient.h's own
        // #defines). Logged right here (throttled to the same cadence as
        // the reconnect attempt itself), not on every loop() iteration,
        // to avoid flooding the log while waiting out the throttle.
        Serial.printf("[MqttClient] Disconnected, PubSubClient state=%d\n", mqttClient_.state());
        lastReconnectAttempt_ = now;
        // Zaehlen, nicht nur loggen: der serielle Monitor setzt dieses Board
        // beim Anstecken zurueck, also ist das Log kein brauchbarer Zeuge.
        // Ein Verbindungsversuch blockiert die einfaedige Schleife rund eine
        // Sekunde - flattert die Verbindung, fuehlt sich das Panel genau so
        // an, wie es der Nutzer beschreibt: meistens fluessig, dann haengt es.
        lastDisconnectState_ = mqttClient_.state();
        reconnect();

        // Widen the wait while it keeps failing, and snap back on success.
        // Without this the loop pays the full socket timeout every five
        // seconds for as long as the broker is away, which on a vehicle is
        // most of the time the panel is switched on.
        if (mqttClient_.connected())
        {
          reconnectBackoffMs_ = RECONNECT_INTERVAL_MS;
        }
        else if (reconnectBackoffMs_ < RECONNECT_BACKOFF_MAX_MS)
        {
          reconnectBackoffMs_ *= 2;
          if (reconnectBackoffMs_ > RECONNECT_BACKOFF_MAX_MS) reconnectBackoffMs_ = RECONNECT_BACKOFF_MAX_MS;
        }
      }
    } else {
      // Process incoming messages - all that have arrived, not one.
      // PubSubClient::loop() reads at most one packet a call, and the board
      // calls this once a loop turn, which on the 4.3B includes a render of
      // 170 ms. Values that queued up while a slider was dragged - ten a
      // second, each echoed back as state - then came out one per render
      // after the finger lifted: the whole drag replayed on the screen before
      // it settled (2026-09-27, in the van). Taken together, the values land
      // first and the next render draws only the newest. Bounded, so a
      // broker that keeps talking cannot hold the loop.
      mqttClient_.loop();
      for (int i = 0; i < 32 && wifiClient_.available() > 0 && mqttClient_.connected(); i++) {
        mqttClient_.loop();
      }
    }
  }
}

void MqttClient::reconnect() {
  // Ohne WLAN gar nicht erst antreten - und vor allem nicht mitzaehlen. Der
  // Zaehler soll "so oft hat das Geraet wirklich beim Broker angeklopft"
  // heissen, nicht "so oft ist die 5-Sekunden-Uhr abgelaufen"; siehe
  // MqttClient.h zu dem Tag, an dem die Zahl in die Irre fuehrte.
  if (!WiFi.isConnected()) return;
  reconnectCount_++;
  connect();
}

void MqttClient::staticCallback(char* topic, byte* payload, unsigned int length) {
  if (instance_) {
    instance_->handleMessage(topic, payload, length);
  }
}

void MqttClient::handleMessage(char* topic, byte* payload, unsigned int length) {
  // Convert payload to String
  String payloadStr;
  payloadStr.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    payloadStr += (char)payload[i];
  }
  
  // Call user callback
  if (messageCallback_) {
    messageCallback_(String(topic), payloadStr);
  }
}

