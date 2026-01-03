from scipy.signal import kaiserord, firwin

# Specifications
# radio filter
fs = 2400000.0       # 2.4 MHz sampling rate
cutoff = 100000.0    # 100 kHz cutoff
width = 30000.0      # 30 kHz transition width
ripple_db = 70.0     # High suppression (60-80dB) for radio signals

# audio/anti-aliasing filter
fs = 480000.0       # 480 kHz sampling rate
cutoff = 15000.0    # 15 kHz cutoff
width = 3650.0      # 5 kHz transition width
ripple_db = 69.0    # High suppression (60-80dB) for radio signals


# Calculate parameters
nyq_rate = fs / 2.0
numtaps, beta = kaiserord(ripple_db, width / nyq_rate)

# Generate coefficients
taps = firwin(numtaps, cutoff, window=('kaiser', beta), fs=fs)