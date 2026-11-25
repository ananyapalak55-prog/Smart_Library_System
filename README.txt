================================================================================
  SMART LIBRARY MANAGEMENT & BOOK LOCATOR SYSTEM (2-LAPTOP NETWORK EDITION)
================================================================================

1. INCLUDED FILES:
   - smart_library_network.cpp   : Complete C++17 Client-Server source code
   - books_catalog.xls           : Central Book Catalog & Stock Excel file
   - help_records.xls            : Saved Student Help Notifications Excel log
   - issued_records.csv          : Active Book Issues & Due Date tracking
   - run_windows.bat             : Double-click to compile & run on Windows
   - run_mac_linux.sh            : Run in terminal on Mac/Linux (chmod +x run_mac_linux.sh)

2. HOW TO RUN ON 2 LAPTOPS OVER MOBILE HOTSPOT:
   Step A: Connect BOTH Laptop 1 and Laptop 2 to the SAME Mobile Hotspot / Wi-Fi.

   Step B: On LAPTOP 1 (Librarian Desk + Central Server):
           1. Open Command Prompt / PowerShell in this folder.
           2. Run 'ipconfig' (Windows) or 'ifconfig' (Mac/Linux) and note the IPv4 Address
              (e.g., 192.168.43.10).
           3. Double-click 'run_windows.bat' OR run:
              g++ -std=c++17 smart_library_network.cpp -o smart_library.exe -lws2_32
              ./smart_library.exe
           4. Choose Option [1] (LAPTOP 1: Librarian Desk + Central Server).
           5. Click "Allow Access" if Windows Firewall pops up.
           6. Login with any Fixed Terminal ID:
              - Floor  0 : F0_LIB1  (Shelves 01-15) | F0_LIB2  (Shelves 16-30)
              - Floor -1 : FM1_LIB1 (Shelves 01-15) | FM1_LIB2 (Shelves 16-30)
              - Floor -2 : FM2_LIB1 (Shelves 01-15) | FM2_LIB2 (Shelves 16-30)
              - Password for all terminals: 1234

   Step C: On LAPTOP 2 (Student Search & Help Kiosk - No Login Required):
           1. Compile and run the program on Laptop 2.
           2. Choose Option [2] (LAPTOP 2: Student Search & Help Kiosk).
           3. Enter Laptop 1's IPv4 Address (e.g., 192.168.43.10).
           4. Select Floor (0, -1, or -2) and Kiosk Computer No (1 to 6).
           5. Search any book (e.g., BK101, BK102, Algorithms) and press [H] for
              instant live help notification on Laptop 1!
================================================================================
