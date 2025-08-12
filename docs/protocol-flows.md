Protocol Flows

Login Flow with Registry Flip

sequenceDiagram
participant C as Client
participant IO as I/O Thread
participant S as Server Thread

      Note over C,S: Connection established, HANDSHAKING state
      Note over C,S: (Singleplayer: TCP localhost; Vanilla uses in-process LocalChannel)

      C->>IO: Handshake(protocol=760, nextState=2)
      IO->>IO: Decode packet
      IO->>IO: Switch to LOGIN registry
      Note over IO: HandshakePacketListener → LoginPacketListener
      IO->>S: Enqueue HandshakePacket

      S->>S: tick() - drain packet queue
      S->>S: Process handshake

      C->>IO: LoginStart(name="Steve")
      Note over IO: Using LOGIN packet registry
      IO->>IO: Decode LoginStart (0x05)
      IO->>S: Enqueue LoginStartPacket

      S->>S: tick() - drain packet queue
      S->>S: Authenticate player
      S->>IO: Enqueue SetCompression(threshold=256)
      S->>IO: Enqueue LoginSuccess(uuid, name)

      IO->>C: SetCompression(256)
      IO->>IO: Install compression pipeline
      Note over IO: All subsequent packets compressed
      IO->>C: LoginSuccess (compressed)

      IO->>IO: Switch to PLAY registry
      Note over IO: LoginPacketListener → PlayPacketListener

      S->>IO: Enqueue initial chunks
      IO->>C: ChunkDataS2C (compressed)
      IO->>C: ChunkDataS2C (compressed)

      Note over C,S: PLAY state active, game begins

Chunk Streaming + Meshing Pipeline

sequenceDiagram
participant P as Player Movement
participant S as Server Thread
participant SW as Server Workers
participant IO as I/O Thread
participant C as Client Thread
participant CW as Client Workers
participant G as GPU

      P->>S: PlayerMoveC2S(newPosition)
      S->>S: Update player chunk position
      S->>S: Calculate required chunks
      S->>SW: Request chunk loading (priority order)

      SW->>SW: Load chunk from .mca file
      SW->>S: Submit ChunkLoadResult

      S->>S: tick() - process loaded chunk
      S->>S: Add to World, create ChunkDataS2C
      S->>IO: Enqueue ChunkDataS2C packet

      IO->>IO: Compress packet (above threshold)
      IO->>C: Send ChunkDataS2C over network

      C->>C: Receive and decode packet
      C->>C: ClientChunkManager.ProcessChunkDataS2CPacket()
      Note over C: Chunk state: UNLOADED → LOADED

      C->>C: ClientMeshManager.ScheduleChunkMeshBuilds()
      C->>CW: Submit mesh build jobs (priority)

      CW->>CW: Greedy meshing algorithm
      CW->>C: Submit MeshBuildResult

      C->>C: ProcessMeshBuildResults()
      C->>G: Upload VBO/IBO data (time budget)
      C->>C: Mark section as ready for rendering

      Note over C,G: Chunk visible in world

Chunk Unload Flow

sequenceDiagram
participant P as Player Movement
participant S as Server Thread
participant IO as I/O Thread
participant C as Client Thread
participant G as GPU

      P->>S: PlayerMoveC2S(farFromChunk)
      S->>S: Update player watch set
      S->>S: Chunk no longer in range

      S->>S: Remove from player's loaded chunks
      S->>IO: Enqueue UnloadChunkS2C(chunkX, chunkZ)

      IO->>C: Send UnloadChunkS2C

      C->>C: ClientChunkManager.ProcessChunkUnloadPacket()
      Note over C: Chunk state: LOADED → UNLOADED

      C->>C: Cancel pending mesh builds
      C->>C: ClientMeshManager.RemoveChunkGPUData()
      C->>G: Delete VBO/IBO resources
      C->>C: Free chunk memory

      Note over C: Chunk removed from client

Block Change → Re-mesh Flow

sequenceDiagram
participant P as Player Action
participant C as Client Thread
participant IO as I/O Thread
participant S as Server Thread
participant CW as Client Workers
participant G as GPU

      P->>C: Block break/place input
      C->>C: Raycast to find target block
      C->>IO: Send BlockActionC2S(x,y,z,action)

      IO->>S: Forward decoded packet

      S->>S: tick() - process BlockActionC2S
      S->>S: Validate action (distance, permissions)
      S->>S: World.SetBlock(x,y,z,newBlockId)
      S->>S: Mark chunk section dirty

      S->>IO: Enqueue BlockChangeS2C(x,y,z,blockId)

      IO->>C: Send BlockChangeS2C

      C->>C: Process block change packet
      C->>C: Update local chunk data
      C->>C: ClientChunkManager.MarkSectionDirty()

      C->>C: ClientMeshManager.ScheduleSectionMeshBuild()
      C->>CW: Submit mesh rebuild for affected section

      CW->>CW: Rebuild section mesh (greedy algorithm)
      CW->>C: Submit MeshBuildResult

      C->>C: Process mesh result
      C->>G: Update existing VBO/IBO data
      C->>G: Re-upload section mesh

      Note over C,G: Block change visible immediately

Keep-Alive Flow

sequenceDiagram
participant S as Server Thread
participant IO as I/O Thread
participant C as Client

      loop Every 15 seconds
          S->>S: Generate keep-alive ID
          S->>IO: Enqueue KeepAliveS2C(id)
          IO->>C: Send KeepAlive

          C->>IO: KeepAliveC2S(id) response
          IO->>S: Forward keep-alive response

          S->>S: Validate keep-alive ID matches
          S->>S: Update last activity time

          alt Keep-alive timeout (30s)
              S->>IO: Enqueue Disconnect("Timed out")
              IO->>C: Send Disconnect
              IO->>IO: Close connection
          end
      end

Protocol State Transitions

stateDiagram-v2
[*] --> HANDSHAKING: TCP connection established

      HANDSHAKING --> STATUS: Handshake(nextState=1)
      HANDSHAKING --> LOGIN: Handshake(nextState=2)

      STATUS --> [*]: Disconnect after ping/pong

      LOGIN --> PLAY: LoginSuccess sent
      LOGIN --> [*]: Login failure

      PLAY --> [*]: Disconnect or connection error

      note right of HANDSHAKING: HandshakePacketListener
      note right of LOGIN: LoginPacketListener
      note right of PLAY: PlayPacketListener
      note right of STATUS: StatusPacketListener

Pipeline Changes During State Transitions

1. HANDSHAKING → LOGIN: Packet listener swapped on I/O thread
2. LOGIN → PLAY: Compression enabled + listener swap
3. SetCompression: Compression pipeline installed immediately
4. Future Encryption: AES/CFB8 pipeline would be installed similarly

All state transitions and pipeline modifications happen on the I/O thread to ensure thread safety and prevent race conditions with ongoing packet processing.
