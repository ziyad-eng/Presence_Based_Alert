This is a Presence Based Alert System. 
The project is designed to be used indoors with a common wifi connection.
Ensure that both your PC and the microcontrollers are connected to the same wifi network.
In the .ino files you can find the variables "WIFI_SSID" and "WIFI_PASS". 

In the audio and image titled files, you can find folders named: Person1, Person2, Person3. 
Ensure you rename those files to the people that you would like enrolled into the system, accordingly.
You can also add extra people into it by simply creating new folders for them.


Go to the "data_collection_server.py" code if you do not have images of the person in the images folder the system you can simply set it up by running the "data_collection_server.py". But before this, you need to upload the data_collection.ino into the AI-Thinker ESP32 board, and run from there. Run the server before and then ensure you turn on the board after to connect to the server. 

The images will be stored in the images folder, but not under any of the person's folder, so take pictures of one person and move their pictures into their respective folder so that the image recognition model can classify them

TARGET_NAMES = ["Person1", "Person2", "Person3"] in this field in the "Server_main.py" also enumerate accordingly, your rename of the folders. If you included any extra names, ensure that they are also in this list. 

The project has dependencies that it needs to run. 

-install for the server.py
-Python3
-flask package
-insight face
-faster-whisper

-install libraries for the esp32s
-esp32cam by Junxiao shi

-install the ESP32 Board
-https://espressif.github.io/arduino-esp32/package_esp32_index.json


AI-Thinker ESP32 CAM 
ESP32 Devkitv1


You can take a quick look at the png to understand the basic flow of data between the two ESP32s
