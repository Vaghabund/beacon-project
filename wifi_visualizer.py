import json
import math
import hashlib
import colorsys
from PIL import Image, ImageDraw
import numpy as np

class WiFiNetwork:
    def __init__(self, ssid, bssid, frequency, security, rssi):
        self.ssid = ssid
        self.bssid = bssid
        self.frequency = frequency
        self.security = security
        self.rssi = rssi

    def to_metadata_string(self):
        """Encode metadata as a compact string."""
        return f"SSID:{self.ssid}|BSSID:{self.bssid}|FREQ:{self.frequency}|SEC:{self.security}"

    def to_bytes(self):
        """Convert metadata string to raw bytes."""
        return self.to_metadata_string().encode('utf-8')  # Raw bytes of the string

    def normalize_rssi(self, min_rssi=-90, max_rssi=-30):
        """Normalize RSSI to a 0-1 range."""
        return max(0, min(1, (self.rssi - min_rssi) / (max_rssi - min_rssi)))

    def get_hue(self):
        """Generate a deterministic hue based on the SSID."""
        hash_value = int(hashlib.md5(self.ssid.encode('utf-8')).hexdigest(), 16)
        return (hash_value % 360) / 360.0

class WiFiVisualizer:
    def __init__(self, canvas_size=1000):
        self.canvas_size = canvas_size
        self.center = canvas_size // 2
        self.image = Image.new("RGBA", (canvas_size, canvas_size), (0, 0, 0, 255))
        self.draw = ImageDraw.Draw(self.image)

    def visualize(self, wifi_networks, background_path, output_path):
        """Generate the visualization and save it as a PNG image."""
        # Load background image and prepare base RGBA image
        background = Image.open(background_path).resize((self.canvas_size, self.canvas_size)).convert("RGBA")
        self.image = background.copy()

        # Sort networks by normalized strength (strongest first)
        sorted_networks = sorted(wifi_networks, key=lambda n: n.normalize_rssi(), reverse=True)

        # Max radius for outermost ring (leave padding)
        max_radius = (self.canvas_size // 2) - 50
        min_radius = 30  # minimum radius for the strongest network

        n_networks = len(sorted_networks)
        if n_networks == 0:
            self.image.save(output_path, "PNG")
            return

        # Compute ring spacing so rings increase in diameter outward
        gap = (max_radius - min_radius) / max(1, (n_networks - 1))

        # Precompute radii for each network (index 0 = strongest)
        radii = []
        for i, net in enumerate(sorted_networks):
            radii.append((net, min_radius + i * gap))

        # Draw from outermost to innermost (weakest first) so inner rings composite on top
        for net, ring_radius in reversed(radii):
            normalized_strength = net.normalize_rssi()
            hue = net.get_hue()
            bytes_data = net.to_bytes()

            # Thickness and pixel size
            ring_thickness = max(15, int(40 * normalized_strength))
            pixel_radius = max(5, int(ring_thickness / 2))

            # Number of pix around ring proportional to strength
            num_pixels = max(30, int(120 * normalized_strength))

            # Debug output
            print(f"SSID: {net.ssid}, RSSI: {net.rssi}, Normalized: {normalized_strength:.3f}, Radius: {ring_radius:.1f}, Pixels: {num_pixels}")

            # Create an overlay for this ring and draw into it, then composite
            overlay = Image.new("RGBA", (self.canvas_size, self.canvas_size), (0, 0, 0, 0))
            overlay_draw = ImageDraw.Draw(overlay)

            for i in range(num_pixels):
                angle_rad = math.radians((360.0 / num_pixels) * i)
                byte_value = bytes_data[i % len(bytes_data)]

                r, g, b = self._byte_to_rgb(byte_value, hue, normalized_strength)
                # Set alpha relative to strength (min 60)
                alpha = int(max(60, 220 * normalized_strength))

                x = int(self.center + ring_radius * math.cos(angle_rad))
                y = int(self.center + ring_radius * math.sin(angle_rad))

                if i < 3:
                    print(f"  Pixel {i}: x={x}, y={y}")

                # Draw filled circle on overlay
                bbox = [x - pixel_radius, y - pixel_radius, x + pixel_radius, y + pixel_radius]
                overlay_draw.ellipse(bbox, fill=(r, g, b, alpha))

            # Composite this ring onto the base image
            self.image = Image.alpha_composite(self.image, overlay)

        # Save the final image
        self.image.save(output_path, "PNG")

    def _byte_to_rgb(self, byte_value, hue, saturation):
        """Convert a byte value to an RGB color using hue and normalized strength (0-1).
        The third parameter is treated as normalized_strength.
        """
        strength = saturation
        sat = 0.5 + 0.5 * strength
        # Baseline brightness plus modulation from the byte value and signal strength
        v = max(0.15, (byte_value / 255.0) * (0.6 + 0.4 * strength))
        rgb = tuple(int(c * 255) for c in colorsys.hsv_to_rgb(hue, sat, v))
        return rgb

    def _draw_circle(self, x, y, radius, color):
        """Draw a circle at position (x, y) with given radius and color."""
        draw = ImageDraw.Draw(self.image)
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=color)

# Example usage
if __name__ == "__main__":
    # Load WiFi scan data
    with open("mock-data/scan-data_1", "r") as f:
        scan_data = json.load(f)

    wifi_networks = [
        WiFiNetwork(
            ssid=network.get("ssid", "<hidden>"),
            bssid=network.get("bssid", "unknown"),
            frequency=network.get("frequency_mhz", "unknown"),
            security=network.get("security", "unknown"),
            rssi=network.get("rssi_dbm", -100)
        )
        for network in scan_data.get("networks", [])
    ]

    # Create visualizer and generate visualization
    visualizer = WiFiVisualizer()
    visualizer.visualize(
        wifi_networks,
        background_path="sample-images/mountains.jpg",
        output_path="output/wifi_visualization.png"
    )