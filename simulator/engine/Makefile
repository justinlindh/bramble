CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -O2 -DBRAMBLE_SIM \
         -I../../test/stubs \
         -I../../components/routing/include \
         -I../../components/packet/include \
         -I../../components/dedup/include \
         -I../../components/crypto/include \
         -I../../components/reliability/include \
         -I../../components/fragment/include \
         -I../../components/channel/include \
         -I../../components/security/include \
         -I../../components/airtime/include \
         -I../../components/timesync/include \
         -I../../components/freq_plan/include

SIM_OBJS = sim_event.o sim_random.o sim_emitter.o sim_node.o sim_radio.o sim_scenario.o sim_metrics.o sim_anomaly.o cJSON.o freq_plan.o

all: bramble-sim

bramble-sim: $(SIM_OBJS) main.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $<

# sim_radio.c reads the frequency plan's default PHY (radio_default_sf).
freq_plan.o: ../../components/freq_plan/freq_plan.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o bramble-sim

.PHONY: all clean
