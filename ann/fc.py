"""
Train a simple fully-connected (FC) network on bit-level data from base64-encoded bodies stored in JSON.
The model will take a *fixed-size* 256-bit prefix as input and predict the *next bit*.

Dataset logic:
  For each base64 entry, decode to bits. Let L = len(bits).
  - For i in [0..(L-2)]:
      prefix = bits[: i+1]
      if len(prefix) < 256:
        # pad with zeros on the left
        prefix = [0]*(256 - len(prefix)) + prefix
      else:
        prefix = prefix[-256:]  # take the last 256 bits if prefix is longer
      label = bits[i+1]

This ensures we can predict from the very beginning of the sequence, i.e., 
the first sample has a single bit in prefix plus 255 zeros on the left, etc.

Usage:
  python train_fc.py --json_path data.json

Make sure you have:
  pip install torch torchvision
"""

import argparse
import json
import base64
import torch
import torch.nn as nn
import tqdm
import random
import os
from torch.utils.data import Dataset, DataLoader, random_split

# =========================
# 1. Dataset Class
# =========================
class Base64BitsFCDataset(Dataset):
    """
    A dataset that:
      1. Reads a JSON file where each entry has {"content": "base64-string"}.
      2. Decodes the base64-encoded body into bytes.
      3. Converts bytes to a list of bits (0 or 1).
      4. For each bit sequence of length L:
          - Generate (L-1) samples.
          - For each i in [0..(L-2)]:
             * prefix = bits[: i+1]
               - If len(prefix) < 256, left-pad with zeros to 256 bits.
               - Else, take prefix[-256:] (last 256 bits).
             * label = bits[i+1] (the "next bit").
    """
    def __init__(self, json_path, seq_length=256):
        super().__init__()
        self.seq_length = seq_length
        
        # Load JSON
        with open(json_path, 'r') as f:
            data = json.load(f)
        
        all_samples = []
        for item in tqdm.tqdm(data, desc="Processing JSON", unit="item"):
            if "content" not in item:
                # Skip malformed entries
                continue
            if "special" in item and item["special"]:
                # Skip special entries (if that's required in your logic)
                continue

            b64body = item["content"]
            
            # Decode from base64 into raw bytes
            try:
                body_bytes = base64.b64decode(b64body)
            except Exception:
                # Skip if there's a decoding error
                continue
            
            # Convert each byte to 8 bits
            bits = []
            for byte in body_bytes:
                for i in range(8):
                    bit = (byte >> (7 - i)) & 1
                    bits.append(bit)

            # Skip empty
            if len(bits) <= 1:
                # If we have 0 or 1 bit, we can't produce a "next bit".
                continue

            # Create samples from the very beginning up to L-2
            # because the label is bits[i+1].
            L = len(bits)
            for i in range(L - 1):
                # prefix includes bits up to index i (so length i+1)
                prefix = bits[: i+1]
                # label is the next bit
                label_bit = bits[i+1]

                # Now pad or truncate prefix to length seq_length
                if len(prefix) < self.seq_length:
                    # left-pad with zeros
                    padded_prefix = [0]*(self.seq_length - len(prefix)) + prefix
                else:
                    # take the last 256 bits
                    padded_prefix = prefix[-self.seq_length:]

                # Store it
                all_samples.append((padded_prefix, label_bit))

        self.samples = all_samples
        print(f"Total samples: {len(self.samples)}")

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, idx):
        """
        Returns:
          x: the 256-bit prefix (padded/truncated if needed) (shape: [256])
          y: the next bit (shape: [1])
        """
        prefix, label_bit = self.samples[idx]
        
        x_tensor = torch.tensor(prefix, dtype=torch.float32)
        y_tensor = torch.tensor(label_bit, dtype=torch.float32)
        return x_tensor, y_tensor


# =========================
# 2. Fully-Connected Model
# =========================
class BitFC(nn.Module):
    """
    A simple fully-connected network for bit prediction.
    Input size = seq_length (256).
    Output size = 1 (logit for next bit).
    """
    def __init__(self, seq_length=256, hidden_size=128):
        super().__init__()
        self.seq_length = seq_length
        self.hidden_size = hidden_size
        
        self.net = nn.Sequential(
            nn.Linear(seq_length, hidden_size),
            nn.ReLU(),
            nn.Linear(hidden_size, hidden_size),
            nn.ReLU(),
            nn.Linear(hidden_size, 1)  # single output for the next bit
        )

    def forward(self, x):
        """
        Args:
          x: shape (batch_size, seq_length)
        Returns:
          logits: shape (batch_size, 1)
        """
        return self.net(x)


# =========================
# 3. Training & Evaluation
# =========================
def train_one_epoch(model, dataloader, criterion, optimizer, device="cpu"):
    model.train()
    total_loss = 0.0
    for x, y in tqdm.tqdm(dataloader, desc="Training", unit="batch"):
        x = x.to(device)
        y = y.to(device).unsqueeze(-1)  # shape: (batch, 1)

        optimizer.zero_grad()
        logits = model(x)  # (batch, 1)
        loss = criterion(logits, y)
        loss.backward()
        optimizer.step()

        total_loss += loss.item()

    return total_loss / len(dataloader)


def evaluate(model, dataloader, criterion, device="cpu"):
    model.eval()
    total_loss = 0.0
    with torch.no_grad():
        for x, y in tqdm.tqdm(dataloader, desc="Evaluating", unit="batch"):
            x = x.to(device)
            y = y.to(device).unsqueeze(-1)

            logits = model(x)
            loss = criterion(logits, y)
            total_loss += loss.item()

    return total_loss / len(dataloader)


# =========================
# 4. Main Script
# =========================
def main(args):
    # Hyperparameters
    json_path = args.json_path
    seq_length = 256
    batch_size = 64
    hidden_size = 16
    num_epochs = 100
    lr = 1e-3
    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"Using device: {device}")

    # 4.1 Create dataset & dataloader
    dataset = Base64BitsFCDataset(json_path, seq_length=seq_length)
    if len(dataset) == 0:
        print("Dataset is empty or insufficient. Exiting...")
        return

    # 4.2 Split dataset into train/val
    val_ratio = 0.1
    val_size = int(len(dataset) * val_ratio)
    train_size = len(dataset) - val_size
    train_dataset, val_dataset = random_split(dataset, [train_size, val_size])

    train_loader = DataLoader(train_dataset, batch_size=batch_size, shuffle=True)
    val_loader = DataLoader(val_dataset, batch_size=batch_size, shuffle=False)

    # 4.3 Instantiate model, loss, optimizer
    model = BitFC(seq_length=seq_length, hidden_size=hidden_size).to(device)

    best_checkpoint_path = "best_bit_fc_model.pth"
    criterion = nn.BCEWithLogitsLoss()
    optimizer = torch.optim.Adam(model.parameters(), lr=lr)

    best_val_loss = float('inf')

    # 4.4 Train
    for epoch in range(num_epochs):
        print(f"Epoch {epoch+1}/{num_epochs}")
        train_loss = train_one_epoch(model, train_loader, criterion, optimizer, device=device)
        val_loss = evaluate(model, val_loader, criterion, device=device)

        print(f"  Train Loss: {train_loss:.4f} | Val Loss: {val_loss:.4f}")

        # Check if this is the best model so far
        if val_loss < best_val_loss:
            best_val_loss = val_loss
            torch.save(model.state_dict(), best_checkpoint_path)
            print(f"  [*] New best model saved with val_loss={val_loss:.4f}")

    print(f"Training complete. Best validation loss: {best_val_loss:.4f}")
    print(f"Best model is stored at '{best_checkpoint_path}'")

    # 4.5 Load best model and export TorchScript
    model.load_state_dict(torch.load(best_checkpoint_path, map_location=device))
    model.eval()
    model.to("cpu")

    # We'll feed an example of the correct shape to script
    # Example input shape: [batch=1, seq_length]
    example_input = torch.zeros((1, seq_length), dtype=torch.float32)
    
    # Script the model
    scripted_model = torch.jit.script(model, example_inputs=(example_input,))
    
    # Save the TorchScript model
    torchscript_path = "best_bit_fc_model_jit.pt"
    scripted_model.save(torchscript_path)
    print(f"Saved TorchScript model to: {torchscript_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--json_path", type=str, required=True,
                        help="Path to the JSON file containing base64-encoded bodies.")
    args = parser.parse_args()
    
    main(args)
