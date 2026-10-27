<h1>Inferno Sentinel Mesh</h1>

<p>
Inferno Sentinel Mesh is a low-cost early warning system for floods, storms, rain and ground erosion. It is built from three small ESP32 boards that talk to each other directly over radio, so it keeps working when the power grid is down and there is no internet. If internet is available, it uses it to add weather forecasts on top, but it never depends on it.
</p>

<p>
Made by Team Inferno. It won the Junior Project Display segment at IUT Techathon Nationals and Rover Summit 2026.
</p>

<br>
<hr>
<br>

<h2>What it does</h2>

<p>
Two sensor nodes are placed out in the field. One watches the weather (temperature, humidity and air pressure), the other watches the ground and the water (how much the ground tilts and how far away the water surface is). Every few seconds they send their readings to a receiver unit.
</p>

<p>
The receiver sits wherever people actually are. It shows the latest readings on a small screen, and when something looks dangerous it switches on a buzzer and an LED. Every type of danger has its own sound and blink pattern, so you can tell what is happening without even looking at the screen.
</p>

<br>
<hr>
<br>

<h2>Why it is useful</h2>

<p>
Most warning systems need either mains power, internet, or an expensive commercial setup. This one needs none of those for the main alerts. It runs on cheap off-the-shelf parts, the nodes are small enough to place almost anywhere, and anyone can understand an alert just by hearing it.
</p>

<p>
It also gives warning from the actual location instead of a general city-wide forecast, since the sensors are measuring the real conditions right where they are placed.
</p>

<br>
<hr>
<br>

<h2>How it works</h2>

<p>
The sensor nodes constantly compare their readings against what is normal. They do not just check one number, they check how fast things are changing. A sudden drop in air pressure points to a storm coming, a quick rise in humidity points to rain, a tilt in the ground points to erosion, and water getting closer to the sonar sensor points to a flood.
</p>

<p>
When a node decides something is wrong, it includes that in the data it sends. The nodes talk to the receiver using ESP-NOW, a direct radio link between ESP32 boards. This means there is no router, no Wi-Fi network and no internet involved in the part that matters. The receiver collects everything, picks the most serious alert if there are several, and shows it.
</p>

<p>
When several alerts happen together, the receiver puts them in order of importance: flood first, then storm, then erosion, then rain.
</p>

<br>
<hr>
<br>

<h2>What it detects</h2>

<p>
<b>Flood:</b> the water level rises compared to the starting level you set when installing it.<br>
<b>Storm:</b> air pressure falls quickly over a short period.<br>
<b>Erosion:</b> the ground or slope tilts past a set limit.<br>
<b>Rain:</b> humidity rises quickly over a short period.
</p>

<br>
<hr>
<br>

<h2>Optional internet features</h2>

<p>
If a laptop is connected to the same network, a small program running on it checks the weather forecast regularly and looks for signs of floods or cyclones. The receiver then picks that up and shows it alongside the live sensor data. This gives an early heads-up before the local sensors even notice anything, but if the laptop or the internet goes away, everything else keeps working as normal.
</p>

<br>
<hr>
<br>

<h2>What it is made of</h2>

<p>
Three ESP32 boards, a temperature/humidity/pressure sensor, a tilt sensor, an ultrasonic distance sensor, a small OLED screen, a buzzer and an LED. Nothing special or hard to find.
</p>

<br>
<hr>
<br>

<h2>Current state</h2>

<p>
The system works and the laptop forecast part is reliable. There is one known problem: when the receiver connects to a Wi-Fi router, it can end up on a different radio channel than the sensor nodes, and the nodes stop reaching it. The Wi-Fi on the sensor nodes also makes them a bit unstable.
</p>

<br>
<hr>
<br>
<h2>Current state</h2> 
<p> The system works end to end. When everything is on the same Wi-Fi network, the sensor nodes, the receiver and the laptop forecast server all work together without any issues. </p>
<br> 
<hr> 
<br>

<h2>What is next</h2>

<p>
The plan is to remove Wi-Fi from the sensor nodes completely so they only use the direct radio link, make the receiver pick up the nodes automatically without typing any addresses, and add a simple web page on the receiver where you can see live readings and change the alert limits.
</p>

<br>
<hr>
<br>


