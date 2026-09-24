"""Reference Snake on an 8x8 board with the machine's exact rules."""


class Snake:
    DIRS = {0: (0, -1), 1: (1, 0), 2: (0, 1), 3: (-1, 0)}   # N, E, S, W = (dx, dy)
    MAX_LEN = 12                # the machine's FIFO depth: at this length the food is passed over, not eaten

    def __init__(self, head=(3, 3), food=(5, 3), direction=1):
        self.body = [head]          # head first
        self.dir = direction
        self.food = food
        self.dead = False
        self.death = None           # 'wall' | 'collision'
        self.crash = None           # the cell the head crashed into
        self.length = 1

    def step(self, req_dir=None, next_food=None):
        """One machine step. req_dir: direction requested since the last step
        (None keeps the current). next_food: where the food goes if eaten."""
        if self.dead:
            return
        if req_dir is not None:
            self.dir = req_dir
        dx, dy = self.DIRS[self.dir]
        hx, hy = self.body[0]
        nx, ny = hx + dx, hy + dy
        if not (0 <= nx < 8 and 0 <= ny < 8):
            self.dead = True; self.death = 'wall'
            return
        grow = (nx, ny) == self.food and self.length < self.MAX_LEN
        tail = self.body[-1]
        if not grow:
            self.body.pop()
        if (nx, ny) in self.body:
            self.dead = True; self.death = 'collision'; self.crash = (nx, ny)
            return
        self.body.insert(0, (nx, ny))
        if grow:
            self.length += 1
            self.food = next_food

    def board(self):
        return {(x, y) for (x, y) in self.body}
