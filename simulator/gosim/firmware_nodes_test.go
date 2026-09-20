package main

import (
	"reflect"
	"testing"
)

func TestFirmwareNodeSpecInstances(t *testing.T) {
	for count, want := range map[int]int{-2: 1, 0: 1, 1: 1, 3: 3} {
		if got := (firmwareNodeSpec{Count: count}).instances(); got != want {
			t.Errorf("Count %d: got %d instances, want %d", count, got, want)
		}
	}
}

func TestLoadFirmwareNodes(t *testing.T) {
	cases := []struct {
		name string
		json string
		want []firmwareNodeSpec
	}{
		{
			name: "every field round-trips through the json tags",
			json: `{"firmware_nodes": [
				{"type": "firmware", "binary": "emulator/node/build/bramble-node",
				 "count": 3, "positions": [[0,0],[100,0],[50,80]],
				 "label": "pager", "env": {"NODE_ROLE": "gateway"}}
			]}`,
			want: []firmwareNodeSpec{{
				Type:      "firmware",
				Binary:    "emulator/node/build/bramble-node",
				Count:     3,
				Positions: [][2]float32{{0, 0}, {100, 0}, {50, 80}},
				Label:     "pager",
				Env:       map[string]string{"NODE_ROLE": "gateway"},
			}},
		},
		{
			name: "a group with no binary is dropped",
			json: `{"firmware_nodes": [{"binary": "", "count": 5}, {"binary": "node-a"}]}`,
			want: []firmwareNodeSpec{{Binary: "node-a"}},
		},
		{name: "malformed JSON returns nil", json: "not json"},
		{name: "absent firmware_nodes key returns nil", json: `{"nodes": []}`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := loadFirmwareNodes([]byte(tc.json)); !reflect.DeepEqual(got, tc.want) {
				t.Fatalf("got %+v\nwant %+v", got, tc.want)
			}
		})
	}
}
