// steps.ts
//
// The guided tour's script: five steps, in the order a newcomer meets the
// product. Each step owns its copy, the action buttons that drive the real
// fleet, the element it points at, and the predicate that decides when it is
// finished. Everything a step asserts comes from firmware console output
// (milestones.ts), so a step completes because the mesh did the thing, never
// because a timer elapsed.
//
// Steps are data, not components: the overlay renders them and the e2e suite
// reads the same ids, so there is one description of the tour rather than two.

import { ROLE_NAMES, type Fleet, type FleetRole } from './fleet';
import { hasPinned, multiHopReceipt, type Milestones } from './milestones';

// An action button on a step. `kind` is dispatched by the overlay into broker
// commands (TourOverlay.tsx runAction); `needs` lists the roles that must be
// attached before the button can do anything.
export interface TourAction {
  kind:
    | 'provision-pair'
    | 'provision-rest'
    | 'send-channel'
    | 'announce-identity'
    | 'send-dm'
    | 'send-receipt';
  label: string;
  needs: FleetRole[];
}

export interface TourStep {
  id: string;
  title: string;
  // Paragraphs of copy. Plain strings: the overlay renders them as text, so
  // there is no markup to escape and no way for a step to reach into the app.
  body: string[];
  // Which main view the step is about, so the overlay can offer to switch to
  // it rather than leaving the user hunting for the right tab.
  view: 'mesh' | 'devices';
  // data-testid of the real UI element the step points at, resolved against
  // the attached fleet (device cards and consoles are keyed by node id). The
  // overlay rings it from the outside, so no targeted component has to know
  // the tour exists. Null when the element is not on screen yet.
  target(fleet: Fleet): string | null;
  actions: TourAction[];
  // One line telling the user what the tour is waiting to observe.
  waitingFor: string;
  done(m: Milestones, fleet: Fleet): boolean;
  // Live detail under the step, e.g. the relay path a receipt came home by.
  detail(m: Milestones, fleet: Fleet): string;
}

function attached(fleet: Fleet): string[] {
  return Object.values(fleet).filter((v): v is string => v !== null);
}

function heard(list: readonly string[], fleet: Fleet, role: FleetRole): boolean {
  const id = fleet[role];
  return id !== null && list.includes(id);
}

export const TOUR_STEPS: TourStep[] = [
  {
    id: 'orientation',
    title: 'What you are looking at',
    body: [
      'Three pagers on a map. Each one is the real Bramble firmware, compiled for Linux instead of an ESP32-S3 and run as an ordinary process, so the mesh stack, the crypto and the screen you see are the same code that ships to hardware.',
      'What is simulated is the radio. gosim models one LoRa channel: range, time-on-air, collisions and half duplex. Nodes hear each other only when the model says a frame reached them, which is why geometry matters here.',
      'ALPHA and CHARLIE sit 200 units apart with a 150-unit range, so they cannot hear each other at all. Anything they say to one another has to go through BRAVO.',
    ],
    view: 'mesh',
    target: () => 'mesh-canvas',
    actions: [],
    // Readiness is the whole of this step's completion test, and it is also
    // what makes the next step safe to click: a node attaches to the broker
    // early in boot but only registers its control-path handlers once the
    // mesh is up, so a provision sent before then is dropped on the floor.
    // The node says so itself when it is ready.
    waitingFor: 'the three firmware nodes to boot, attach to the ether and open their control path',
    done: (m, fleet) =>
      attached(fleet).length >= 3 && attached(fleet).every((id) => m.controlReady.includes(id)),
    detail: (m, fleet) => {
      const ready = attached(fleet).filter((id) => m.controlReady.includes(id)).length;
      return `${attached(fleet).length} of 3 nodes attached, ${ready} ready to drive`;
    },
  },
  {
    id: 'provision',
    title: 'A fleet with no key is inert',
    body: [
      'Bramble fails closed. A node with no network key does not beacon, does not relay and does not accept traffic: it sits there being a paperweight. Open a console below and you will find the line "unprovisioned: no beacon key (node inert until provisioned)" on all three.',
      'Provisioning is what ends that. Key ALPHA and BRAVO first and watch the pair come alive while CHARLIE stays silent: no beacons from it, and nothing it hears goes anywhere.',
      'Then key CHARLIE and the line completes. The key travels over the emulator control path to the same call the setNetworkKey RPC makes on real hardware, so nothing here is a shortcut around the fail-closed state.',
    ],
    view: 'devices',
    target: (fleet) => (fleet.charlie ? `device-card-${fleet.charlie}` : null),
    actions: [
      { kind: 'provision-pair', label: 'Provision ALPHA and BRAVO', needs: ['alpha', 'bravo'] },
      { kind: 'provision-rest', label: 'Provision CHARLIE', needs: ['charlie'] },
    ],
    waitingFor: 'all three nodes to report a provisioned network key',
    done: (m, fleet) => attached(fleet).every((id) => m.provisioned.includes(id)) && attached(fleet).length >= 3,
    detail: (m, fleet) => {
      const on = attached(fleet).filter((id) => m.provisioned.includes(id)).length;
      return `${on} of ${Math.max(attached(fleet).length, 3)} nodes provisioned`;
    },
  },
  {
    id: 'channel',
    title: 'A channel message, relayed',
    body: [
      'ALPHA broadcasts on the public channel. BRAVO hears it directly, decrypts it, and rebroadcasts it because that is what a mesh node does with a flooded frame it has not seen before.',
      'CHARLIE is out of ALPHA\'s range, so the only copy it can possibly receive is the one BRAVO put back on the air. When the text appears in CHARLIE\'s console, that is a genuine two-hop delivery, not a shortcut.',
      'Watch the map while it happens: the frame animates from ALPHA to BRAVO and then from BRAVO to CHARLIE. A broadcast is Bramble\'s lowest reliability tier: no acknowledgement and no retransmission, so if it collides with another node\'s transmission it is simply gone. Send it again; that is what the tier means.',
    ],
    view: 'mesh',
    target: () => 'mesh-canvas',
    actions: [{ kind: 'send-channel', label: 'Broadcast from ALPHA', needs: ['alpha'] }],
    waitingFor: `${ROLE_NAMES.charlie} to print the broadcast text it could only have got through ${ROLE_NAMES.bravo}`,
    done: (m, fleet) => heard(m.channelHeardBy, fleet, 'charlie'),
    detail: (m, fleet) =>
      heard(m.channelHeardBy, fleet, 'charlie')
        ? 'CHARLIE received the broadcast over two hops'
        : `${m.channelHeardBy.length} node(s) have printed it so far`,
  },
  {
    id: 'dm',
    title: 'A direct message, and the safety number',
    body: [
      'A direct message is not a channel message. ALPHA and BRAVO run a key exchange first and then talk inside a session only those two can read. The handshake goes out at Critical tier, the most persistent of Bramble\'s three reliability tiers (Broadcast, Normal, Critical): eight retries from a 3 s base, because losing a handshake frame stalls the whole conversation.',
      'Encryption alone does not tell you who is on the other end. Bramble derives a 7-digit safety number from both identities, and comparing it out of band is what turns "encrypted" into "encrypted to the person I mean".',
      'A node can only derive that number for a peer whose identity it has PINNED, and a pin comes from an identity attestation: the peer announcing its keys, signed. A node announces once it has a network key and every fifteen minutes after that, and the announcement is an ordinary broadcast that can be lost. Ask ALPHA to announce now rather than waiting on that cadence, and ask again if BRAVO\'s peer screen still says it has no session.',
      'Then do the verification on the device: on BRAVO\'s face press DOWN until the header reads Nodes, press SELECT to open the peer list, DOWN until the cursor sits on ALPHA, and SELECT to open it. The 7 digits are the safety number. SELECT arms the confirmation and one more SELECT commits it, and the peer is marked VERIFIED.',
    ],
    view: 'devices',
    target: (fleet) => (fleet.bravo ? `device-card-${fleet.bravo}` : null),
    actions: [
      { kind: 'announce-identity', label: "Announce ALPHA's identity", needs: ['alpha'] },
      { kind: 'send-dm', label: 'Send a DM from ALPHA to BRAVO', needs: ['alpha', 'bravo'] },
    ],
    waitingFor: 'BRAVO to receive the DM and record a confirmed safety number',
    done: (m, fleet) => heard(m.dmHeardBy, fleet, 'bravo') && heard(m.verified, fleet, 'bravo'),
    detail: (m, fleet) => {
      const pinned = hasPinned(m, fleet.bravo, fleet.alpha);
      const got = heard(m.dmHeardBy, fleet, 'bravo');
      const ver = heard(m.verified, fleet, 'bravo');
      if (got && ver) return 'DM delivered and the safety number confirmed on BRAVO';
      const parts = [
        pinned
          ? "BRAVO pinned ALPHA's identity, so the safety number is on its peer screen"
          : "no identity pin for ALPHA seen on BRAVO yet, so its peer screen reads \"No secure session yet\"",
        got ? 'DM delivered' : 'waiting for the key exchange and the DM to land on BRAVO',
      ];
      return parts.join('; ');
    },
  },
  {
    id: 'receipt',
    title: 'The delivery receipt and its relay path',
    body: [
      'A broadcast asks its recipients for a delivery receipt, and each receipt carries the route it came home by. That is how a sender learns not just that a message arrived but which nodes carried the answer.',
      'CHARLIE is two hops away, so its receipt has to be forwarded by BRAVO, which appends itself to the path on the way through. ALPHA\'s console then prints the receipt with that path in travel order, receiver first.',
      'Receipts are rationed rather than free: recipients answer in spread-out slots to avoid colliding with each other, the whole class is off in meshes above forty peers, and every transmission is charged to the receipt airtime lane.',
    ],
    view: 'devices',
    target: (fleet) => (fleet.alpha ? `console-${fleet.alpha}` : null),
    actions: [{ kind: 'send-receipt', label: 'Broadcast and trace the route', needs: ['alpha'] }],
    waitingFor: 'a delivery receipt to reach ALPHA with more than one hop in its path',
    done: (m) => multiHopReceipt(m) !== null,
    detail: (m) => {
      const r = multiHopReceipt(m);
      if (r) return `receipt from ${r.from} via ${r.path.join(' > ')}`;
      if (m.receipts.length > 0) {
        return `${m.receipts.length} single-hop receipt(s) home; waiting for CHARLIE's`;
      }
      return 'waiting for the first receipt';
    },
  },
];
