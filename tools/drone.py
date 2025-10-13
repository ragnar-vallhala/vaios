import pygame
import math
import serial
import time

# --- Pygame setup ---
WIDTH, HEIGHT = 800, 600
pygame.init()
screen = pygame.display.set_mode((WIDTH, HEIGHT))
pygame.display.set_caption("Drone 3D Orientation Viewer")
clock = pygame.time.Clock()

# --- UART setup ---
ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
time.sleep(2)  # wait for serial connection

# --- 3D drone cube vertices ---
size = 50
vertices = [
    [-size, -size, -size],
    [ size, -size, -size],
    [ size,  size, -size],
    [-size,  size, -size],
    [-size, -size,  size],
    [ size, -size,  size],
    [ size,  size,  size],
    [-size,  size,  size],
]

edges = [
    (0,1),(1,2),(2,3),(3,0),
    (4,5),(5,6),(6,7),(7,4),
    (0,4),(1,5),(2,6),(3,7)
]

# Drone orientation
yaw = 0.0
last_time = time.time()

# --- Helper functions ---
def parse_line(line):
    try:
        acc_str = line.split("ACC:")[1].split("GYR:")[0].strip()
        gyr_str = line.split("GYR:")[1].strip()
        ax, ay, az = [int(x) for x in acc_str.split(',')]
        gx, gy, gz = [int(x) for x in gyr_str.split(',')]
        return ax, ay, az, gx, gy, gz
    except:
        return None

def acc_to_angles(ax, ay, az):
    ax_n = ax / 16384.0
    ay_n = ay / 16384.0
    az_n = az / 16384.0

    pitch = math.atan2(-ax_n, math.sqrt(ay_n*ay_n + az_n*az_n))
    roll  = math.atan2(ay_n, az_n)
    return pitch, roll


def rotate_point(x, y, z, pitch, roll, yaw):
    # Apply rotations: yaw (Z), pitch (X), roll (Y)
    # Pitch around X-axis
    x1 = x
    y1 = y * math.cos(pitch) - z * math.sin(pitch)
    z1 = y * math.sin(pitch) + z * math.cos(pitch)
    # Roll around Y-axis
    x2 = x1 * math.cos(roll) + z1 * math.sin(roll)
    y2 = y1
    z2 = -x1 * math.sin(roll) + z1 * math.cos(roll)
    # Yaw around Z-axis
    x3 = x2 * math.cos(yaw) - y2 * math.sin(yaw)
    y3 = x2 * math.sin(yaw) + y2 * math.cos(yaw)
    z3 = z2
    return x3, y3, z3

def project(x, y, z):
    # Simple perspective projection
    factor = 500 / (z + 500)
    x_proj = x * factor + WIDTH // 2
    y_proj = -y * factor + HEIGHT // 2
    return int(x_proj), int(y_proj)

# --- Main loop ---
while True:
    for event in pygame.event.get():
        if event.type == pygame.QUIT:
            ser.close()
            pygame.quit()
            exit()

    # Read UART
    if ser.in_waiting:
        line = ser.readline().decode('utf-8').strip()
        data = parse_line(line)
        if data:
            ax, ay, az, gx, gy, gz = data
            pitch, roll = acc_to_angles(ax, ay, az)

            dt = time.time() - last_time
            yaw += gz * dt * 0.0007
            last_time = time.time()

            # Rotate and project all vertices
            projected = []
            for v in vertices:
                x, y, z = rotate_point(v[0], v[1], v[2], pitch, roll, yaw)
                projected.append(project(x, y, z))

            # Draw edges
            screen.fill((30,30,30))
            for e in edges:
                pygame.draw.line(screen, (0,255,0), projected[e[0]], projected[e[1]], 3)

    pygame.display.flip()
    clock.tick(60)

