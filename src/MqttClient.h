#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <vector>
#include <map>
#include <functional>
#include "interfaces/IMessagingClient.h"

/**
 * MQTT implementation of IMessagingClient
 * 
 * Responsibilities:
 * - Connect to MQTT broker with credentials
 * - Subscribe to topics
 * - Handle incoming messages via callback
 * - Maintain connection and auto-reconnect
 */
class MqttClient : public IMessagingClient {
public:
  MqttClient();
  
  /**
   * Configure MQTT broker connection
   * @param host Broker hostname or IP
   * @param port Broker port (default 1883)
   * @param username Username for authentication (optional)
   * @param password Password for authentication (optional)
   */
  void configure(const String& host, uint16_t port, const String& username = "", const String& password = "") override;
  
  /**
   * Set the callback for receiving messages
   */
  void setCallback(MessageCallback callback) override;
  
  /**
   * Connect to MQTT broker
   * @return true if connected
   */
  bool connect() override;
  
  /**
   * Check if connected to broker
   */
  bool isConnected() override;
  
  /**
   * Subscribe to a message key
   * @param key Message key to subscribe to
   * @return true if subscription successful
   */
  bool subscribe(const String& key) override;
  
  /**
   * Subscribe to multiple message keys
   * @param keys List of message keys to subscribe to
   */
  void subscribeToKeys(const std::vector<String>& keys) override;
  
  /**
   * Unsubscribe from a message key
   * @param key Message key to unsubscribe from
   * @return true if unsubscription successful
   */
  bool unsubscribe(const String& key) override;
  
  /**
   * Publish an action event
   * @param actionName Name of the action to publish
   */
  bool publishAction(const String& actionName) override;

  /**
   * Publish an arbitrary topic/payload pair
   */
  bool publish(const String& topic, const String& payload, bool retain = false) override;
  
  /**
   * Publish a payload too large for the 1 KB buffer - a device's description
   * (about 300 KB), its recovery copy - in pieces: begin with the whole
   * length, write it in chunks, end. PubSubClient sends the pieces straight
   * to the socket, so nothing the size of the payload is ever held in RAM
   * (docs/device-contract.md §4, `ddf`).
   */
  bool beginLargePublish(const String& topic, size_t length, bool retain);
  size_t writeLargePublish(const uint8_t* data, size_t length);
  bool endLargePublish();

  /**
   * Clear all subscriptions
   */
  void clearSubscriptions() override;
  
  /**
   * Process incoming messages - must be called regularly in loop()
   */
  void loop() override;

  // Wie oft die Verbindung neu aufgebaut werden musste, und warum sie zuletzt
  // als getrennt galt (PubSubClient::state(): -4 = PINGREQ unbeantwortet,
  // -3 = Socket-Lesefehler). Ueber GET /api/debug lesbar - der serielle
  // Monitor setzt dieses Board beim Anstecken zurueck und taugt nicht als
  // Zeuge fuer ein Problem, das nur im Betrieb auftritt.
  //
  // Bis 2026-08-26 zaehlte reconnectCount_ auch die Versuche, die am
  // WLAN-Test in reconnect() scheiterten - bei totem WLAN also alle 5s
  // einen. "45 Reconnects" waren dann in Wahrheit vier echte Verbindungen
  // und viel Warten; die Zahl trug eine Diagnose, die sie nicht tragen
  // konnte. Jetzt zaehlt sie nur noch Versuche, die das Haus verlassen
  // haben, und connectCount_ davon die erfolgreichen. Die Differenz ist die
  // Zeit in der Luecke, in der nichts zugestellt werden kann.
  uint32_t reconnectCount() const { return reconnectCount_; }
  uint32_t connectCount() const { return connectCount_; }
  int lastDisconnectState() const { return lastDisconnectState_; }

  /**
   * This client's own stable identity ("M5Dial-" + MAC-derived hex) - also
   * used as the MQTT topic namespace segment for the deploy/hello/status
   * topics (see Application::setupMQTT() and DeployManager), so a deploy
   * trigger/discovery message always lines up with the same id this client
   * connects under.
   */
  String getClientId() const { return clientId_; }

  /**
   * Fires every time connect() succeeds - both the initial connect() call
   * and every automatic reconnect via loop()'s reconnect() path, since a
   * dropped connection also means any MQTT Last Will (see connect()'s
   * will-bearing overload below) fired "offline" on the broker, so
   * whatever needs to be freshly (re-)announced - status=online, the
   * "hello" payload - must happen again each time, not just once at boot.
   */
  using ConnectedCallback = std::function<void()>;
  void setConnectedCallback(ConnectedCallback callback) { connectedCallback_ = callback; }

private:
  WiFiClient wifiClient_;
  PubSubClient mqttClient_;
  MessageCallback messageCallback_;
  
  String host_;
  uint16_t port_;
  String username_;
  String password_;
  String clientId_;
  
  std::vector<String> subscribedTopics_;

  // Action counters (one per action name)
  std::map<String, unsigned long> actionCounters_;

  ConnectedCallback connectedCallback_;
  
  // Static callback for PubSubClient (forwards to instance method)
  static void staticCallback(char* topic, byte* payload, unsigned int length);
  static MqttClient* instance_;
  
  // Instance callback
  void handleMessage(char* topic, byte* payload, unsigned int length);
  
  // Auto-reconnect
  void reconnect();
  uint32_t reconnectCount_ = 0;
  uint32_t connectCount_ = 0;
  int lastDisconnectState_ = 0;
  unsigned long lastReconnectAttempt_;
  // The wait before the next attempt, doubling while attempts keep failing
  // and reset the moment one succeeds. A blocked attempt is not free: the
  // TCP connect blocks this single-threaded loop for the socket timeout,
  // measured at 3.0s on the 4.3B against an unreachable broker. Retrying
  // every 5s therefore spent 60% of all time inside this call, and the
  // panel was blind to its own touchscreen for three seconds out of five -
  // twelve of twenty deliberate swipes were never seen at all.
  unsigned long reconnectBackoffMs_ = RECONNECT_INTERVAL_MS;
  static const unsigned long RECONNECT_INTERVAL_MS = 5000;
  // A minute between attempts against a broker that is not there. Nothing
  // is lost by waiting: a broker that comes back stays back, and one
  // attempt in sixty seconds costs 5% of the loop instead of 60%.
  static const unsigned long RECONNECT_BACKOFF_MAX_MS = 60000;
};

