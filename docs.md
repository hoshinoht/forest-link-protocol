CSC2106: IoT Protocols and Networks

Project Proposal Report

Group:  GP01
Name
SIT ID
Po Haoting
2401280
Ong Tun Siang
2402091
Kenny Leck
2403543
Chia Wei Sheng
2400953



Feedback Question (Feb 1, 2026): How does the mqtt work with the ble and lora?

MQTT works together with BLE and LoRa on separate layers.


Internal Mesh (BLE/LoRa): Nodes within the internal mesh communicate with one another using BLE or LoRa.


External Data Exfiltration/Communication (MQTT): When a node detects internet connectivity, it switches to WiFi and uses MQTT to publish buffered data to a cloud broker.

In our problem statement, nodes are deployed in a dense forest environment, which is not ideal for WiFi signals to communicate, and not every node is connected to the internet or WiFi network. Hence, protocols such as BLE and LoRa will be used for communications in the dense forest mesh environment. And only a select few nodes with WiFi will send the data out of the forest mesh.

The Design Specification better visualises our idea.










1. What specific issue or problem has been identified that requires a solution?
The primary issue is the "Green Wall" effect, where the dense, water-heavy foliage of forest environments severely attenuates 2.4GHz WiFi signals. Standard IoT deployments in these areas face a "Bandwidth-Reach Paradox": high-bandwidth protocols (WiFi) cannot penetrate the vegetation over long distances, while long-range protocols (LoRa) lack the throughput necessary to move rich media or large data logs. This results in "disconnected" or "brittle" networks that fail to deliver critical environmental data when signal quality fluctuates.

2. Why is your topic important or relevant to industry, academia, or society?
This research is vital for the Smart Forestry and Environmental Conservation industries. As climate change increases the frequency of wildfires and ecological shifts, society requires real-time, high-fidelity data (images, vibration logs, and acoustics) from "Deep Field" environments. Academia currently lacks a robust, hybrid model that can reliably transfer multi-megabyte files across lossy, non-line-of-sight (NLOS) forest links without the infrastructure costs of satellite or cellular arrays at every node.

3. What is the potential impact of addressing this challenge?
Addressing this challenge enables the deployment of Rich-Media Sensor Networks in remote regions. The potential impact includes:
Enhanced Early Warning Systems: Faster transmission of high-resolution fire detection images.
Ecological Insight: Continuous collection of large-scale biodiversity data (audio/visual) without manual retrieval.
Network Resilience: Providing a fail-safe communication framework that remains "conscious" via adaptive protocol selection even when high-speed links are temporarily obstructed.
4. What specific gap or challenge in the field does this work address?
This work addresses the gap between Low-Power Wide-Area Networks (LPWAN) and High-Speed Wireless Local Area Networks (WLAN).
The Bandwidth Gap: LoRaWAN is restricted by 1% duty cycles and low bitrates, making a 3MB file transfer take days.
The Reliability Gap: Standard TCP/IP meshes suffer from "Management Collapse" in forests; if the WiFi link drops, the entire protocol state fails. 
The Power and Range Trade-off: Existing solutions do not implement adaptive switching between BLE (power-efficient short-range) and LoRa (power-hungry long-range) based on network conditions, forcing them to only use one protocol with relatively low efficiency.

FLP v3.7 fills this gap by using an Adaptive Multi-Protocol Architecture that intelligently switches between BLE and LoRa for internal mesh communications based on power budget, hop count, and RSSI, while reserving Wi-Fi for MQTT-based external data exfiltration when connected to the internet.
5. What are the main goals of this project or research?
The overarching goal is to implement and validate the Forest Link Protocol v3.7, focusing on:
Adaptive Protocol Selection: Developing an intelligent system that can dynamically choose between BLE and LoRa based on real-time power budget, hop counts, and RSSI measurements
Efficient Recovery: Implementing End-to-End (E2E) Selective Repeat logic to fix packet loss locally without full-mesh overhead.
Optimised Reassembly: Utilising the ESP32-S3's 8MB PSRAM for O(1) memory-mapped fragment reassembly, ensuring the hardware does not become a bottleneck during 3MB bursts.
Opportunistic Data Exfiltration: Implementing a Wi-Fi-MQTT publish-subscribe model in which nodes subscribe to an admin topic and automatically exfiltrate buffered data to external servers when internet connectivity is detected.

6. What specific outcomes are expected by the end of this study?
By the conclusion of this study, the following outcomes are expected:
Functional Adaptive Mesh: A stable mesh that dynamically selects between BLE or LoRa for inter-node communications, ensuring optimal energy efficiency and reliability
Successful 3MB Transfers: Verified capability to move a 3MB file across at least three hops in a "Deep Field" forest environment within minutes.
Opportunistic Cloud Sync: Implementation of WiFi-MQTT-based data exfiltration where nodes automatically detect internet availability, subscribe to admin control topics, and push buffered data to external systems without manual intervention.
Secure Throughput: Implementation of Hardware-Accelerated AES-GCM encryption that protects data integrity without significantly reducing effective goodput.











Design Specifications
Overview
The proposed Forest Link Protocol (FLP) is essentially a multi-protocol mesh network that is designed to address the issue of reliable and reasonably fast data transfer over dense forest environments.

The ESP32 S3 nodes are connected using BLE for short, high-bandwidth range communications, and LoRa for long-range communication, with access to the MQTT broker/gateway being opportunistic, i.e. If the Node can connect to the gateway via Wifi, it will do so, and keep note of the RSSI strength in memory.

The ESP32 Nodes are designed to be opportunistic, where they will occasionally attempt to connect to the MQTT Gateway to check if they have access, and if they do, they will update their own state to reflect it.



Design Constraints & Assumptions
This proposed protocol assumes that
Out of N the nodes within the mesh, there is at least one node that is capable of accessing the internet using wifi
The assumption is that in a real-world environment, this ESP32 node is located near a place with internet connection, or mobile data access via 5G/4G

Data transfer does not need to be immediate; however, it should be able to deliver data back to the cloud in a reasonable amount of time, compared to using existing protocols such as LoRa to transfer data over long ranges.
All ESP32 nodes are running an MQTT-SN Client and store a lookup table that is synchronised with the MQTT Admin (cloud) to ensure publishes are sent to the correct nodes. 
Data transfer on the meshes will transfer one file at a time, not multiple

Data Transfer (Forest -> Cloud)
When there is data that needs to be transmitted from an ESP32 node to the internet, it will advertise its intent by broadcasting LoRa packets to the other nodes around it, with the intent of locating and electing nodes with internet connection to the MQTT Admin (in this case, the gateway).



Once the Nodes have been elected for data transfer, they become exit nodes, where they will forward data over to the MQTT Gateway using the lightweight MQTT-SN protocol, and Node A will start to send fragmented data packets over to D and C, using Bluetooth or LoRa, where the choice of protocol selection is based on factors such as Power Budget, Priority, Available connection. If Bluetooth is selected, intermediate nodes will become relays to pass the data over to the exit nodes.


The MQTT-SN adopts a selective repeat sliding window algorithm for flow control and to handle missing packets. Where the MQTT Admin is in charge of 
Reconstructing fragmented packets
Discarding duplicates
Reconstructing the original data (i.e. image, etc.)
Sending NACKs or Control packets to indicate missing packets or progress updates.

Data Transfer (Cloud -> Forest)
Just as this relaying protocol works for transmitting data from forest nodes over to the cloud, the reverse is just as possible, as each ESP32 client will have a local topic lookup table that is stored within its client.

Through this, it is possible for the MQTT Admin to publish MQTT packets back down to ESP32 nodes, and they are capable of being relayed or broadcast back to other nodes who may not have access to the MQTT gateway.

This allows for control of these forest nodes from the cloud, so long as there is at least one node that has access to the MQTT Gateway.

Functional Requirements
[FR-MESH1] The ESP32 nodes must be able to communicate with each other using either BLE or LoRa connections
[FR-MESH2] The ESP32 nodes must store an MQTT topic lookup table that is used to either send data back to the cloud, or receive and propagate topics from the cloud to other nodes
[FR-MESH3] The ESP32 nodes must be capable of interfacing with the MQTT Gateway using UDP via an MQTT-SN Client
[FR-MESH4] The ESP32 nodes must be capable of parsing intent packets and sending back ACK packets notifying the sender if they have access to the MQTT Gateway
[FR-MESH5] The ESP32 nodes must be capable of receiving back MQTT published packets from either the Gateway or forwarded from LoRa/BLE
[FR-MESH6] The ESP32 nodes must be capable of adaptively selecting the appropriate protocol to use to send data packets based on factors such as (Power Budget, Power Efficiency, Packet Size, Priority)
[FR-MESH7] The ESP32 nodes must be able to support the packet broadcast retry logic of a maximum of 3 times
[FR-MESH8] The ESP32 nodes must be able to either forward packets over to the next node, or be able to broadcast and receive packets if they are the intended recipient
[FR-MQTT1] The MQTT Admin must be capable of publishing and subscribing to topics
[FR-MQTT2] The MQTT Admin must be capable of maintaining a selective repeat sliding window during data transfers
[FR-MQTT3] The MQTT Admin will retain a FIFO Queue of pending file transfers, and the Mesh Network shall not transfer multiple files at once. With the MQTT Admin controlling the file transfer flow, to ensure data integrity.
[FR-MQTT4] The ESP32 Node’s MQTT-SN client must be capable of supporting different Quality of Service (QoS) levels, to ensure network reliability 
Non-Functional Requirements
[NFR-MESH1] The Mesh network must be capable of delivering a 1MB file over the mesh into the MQTT cloud within 20 minutes.
[NFR-MESH2] The ESP32 Nodes packet processing must be interrupt-driven, and not polling-driven

