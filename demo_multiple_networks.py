from wifi_visualizer import WiFiNetwork, WiFiVisualizer

if __name__ == "__main__":
    networks = [
        WiFiNetwork(ssid="MeshNet", bssid="de:ad:be:ef:10:01", frequency=2412, security="WPA2-Enterprise", rssi=-30),
        WiFiNetwork(ssid="Coffee_WiFi", bssid="aa:bb:cc:dd:11:22", frequency=2417, security="WPA2-PSK", rssi=-52),
        WiFiNetwork(ssid="EdgeNet_5G", bssid="11:22:33:44:55:66", frequency=5180, security="WPA2", rssi=-65),
        WiFiNetwork(ssid="OpenNet", bssid="66:55:44:33:22:11", frequency=2462, security="Open", rssi=-78),
    ]

    visualizer = WiFiVisualizer()
    visualizer.visualize(
        networks,
        background_path="sample-images/mountains.jpg",
        output_path="output/wifi_demo_multiple.png",
    )
